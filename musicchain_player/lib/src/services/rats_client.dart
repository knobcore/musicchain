// Dart-side wrapper around the librats C API.
//
// Responsibilities:
//   * Own the singleton `rats_client_t` for the app.
//   * Connect to the VPS rendezvous so we end up on the librats mesh.
//   * Provide a small request/response layer on top of librats typed messages
//     so the rest of the app can do `rats.request(peerId, "songs.list", {...})`
//     and await a JSON-decoded reply.
//
// The byte-stream side (`rats.stream(peerId, "<song_hash>")`) is added in a
// later slice.

import 'dart:async';
import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';
import 'package:flutter/services.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../ffi/rats_bindings.dart';

/// Bootstrap mini-node used as the initial peer. The mesh-discovery layer
/// (mini.hello + mininodes.list) grows this into a full list of VPSes
/// once the first handshake completes, so the player can fail over if
/// this one ever goes down.
const String kVpsHost     = '85.239.238.226';
// Vanilla librats's project default port. Peer connections are TCP; native
// STUN + DHT live inside librats on ephemeral UDP sockets we never advertise.
// There's no HTTP/3 surface today — the standalone msh3 listener was
// removed when h3_server was folded into (then ripped out of) the QUIC RPC.
const int    kVpsRatsPort = 8080;

/// UDP port the player binds for the BitTorrent-compatible DHT swarm. 0
/// asks librats to pick an ephemeral one — fine because peers reach the
/// node via the DHT routing table, not a fixed port. We never need to
/// advertise this to a NAT'd peer; the DHT only exists for content-hash
/// → peer-address lookups.
const int    kDhtPort     = 0;

/// A single mini-node we've heard about — either from prefs (cached from
/// a previous run's `mininodes.list` reply) or hard-coded as the
/// bootstrap seed. The watchdog rotates through these whenever the
/// active link drops so a player isn't pinned to one VPS.
class MiniNodeAddr {
  const MiniNodeAddr(this.host, this.port);
  final String host;
  final int    port;

  @override
  bool operator ==(Object other) =>
      other is MiniNodeAddr && other.host == host && other.port == port;
  @override
  int get hashCode => Object.hash(host, port);

  @override
  String toString() => '$host:$port';
}

/// Lowest-effort request/response envelope sent over librats typed messages.
///
/// `req_id` is echoed back so we can match the reply to the awaiting Future.
/// `reply_to` is the librats peer id of the sender; the server fills it in
/// and echoes it back so multi-peer routing stays straight.
class _PendingRequest {
  final Completer<Object?> completer; // body may be Map, List, primitive, null
  final Timer timeout;
  _PendingRequest(this.completer, this.timeout);
}

class RatsClient {
  RatsClient._(this._b, this._handle);

  static RatsClient? _instance;
  static RatsClient get instance {
    final i = _instance;
    if (i == null) {
      throw StateError('RatsClient.initialize() not called');
    }
    return i;
  }

  /// One-shot init at app startup. Subsequent calls are no-ops.
  static Future<RatsClient> initialize({int listenPort = 0}) async {
    if (_instance != null) return _instance!;

    final lib = _loadLibrary();
    final b   = RatsBindings(lib);

    // listenPort=0 lets the OS pick a free ephemeral port — for a player we
    // do not want a fixed port (multiple players on the same LAN, mobile
    // hotspot scenarios, etc.).
    final port = listenPort != 0 ? listenPort : 33000 + Random().nextInt(2000);
    final handle = b.create(port);
    if (handle.address == 0) {
      throw StateError('rats_create($port) returned null');
    }

    final client = RatsClient._(b, handle);
    _instance = client;
    await client._start();
    return client;
  }

  // -- Wiring ------------------------------------------------------------

  final RatsBindings _b;
  final Pointer<Void> _handle;

  String _ownPeerId = '';
  String _publicAddress = '';
  bool _started = false;

  /// Tracks request_id → completer so we can match replies back.
  final Map<String, _PendingRequest> _pending = {};

  /// Tracks stream_id → stream sink so binary chunks can be reassembled.
  final Map<int, _AudioReceiver> _streams = {};

  /// Peer ids that self-identified as a mini-node by sending us a
  /// `mini.hello`. The post-pivot full node dropped the routes.get /
  /// stun.observe surface (those verbs return unknown_type now), so
  /// the helpers that need a mini-node specifically — requestRoutes,
  /// _observePublicAddressViaVps — must route to one of these peers,
  /// NOT to whatever validatedPeerIds.first happens to be. Populated
  /// in _dispatchRequest when an inbound mini.hello arrives.
  final Set<String> _miniNodePeerIds = {};

  /// First currently-validated mini-node peer id, or null if no
  /// mini-node is connected. Prefer this over validatedPeerIds.first
  /// for any RPC only a mini-node answers.
  String? get firstMiniNodePeerId {
    if (!_started) return null;
    final ids = validatedPeerIds;
    for (final id in ids) {
      if (_miniNodePeerIds.contains(id)) return id;
    }
    return null;
  }

  /// Per-target relay routing. Populated by `LibratsDiscovery` from the
  /// `reachability` field returned by routes.get. When set, requests bound
  /// for `targetPeerId` are wrapped as `relay.forward` and sent to the
  /// associated relay peer (the mini-node) instead of being sent direct.
  final Map<String, String> _relayVia = {};

  /// Tell RatsClient that traffic for [targetPeerId] must go via the relay
  /// at [relayPeerId]. Pass `null` to remove a previous mapping.
  void setRelayVia(String targetPeerId, String? relayPeerId) {
    if (relayPeerId == null || relayPeerId.isEmpty) {
      _relayVia.remove(targetPeerId);
    } else {
      _relayVia[targetPeerId] = relayPeerId;
    }
  }

  /// Callback invoked whenever we receive a typed message that is *not* a
  /// reply we were waiting for (i.e. a server-pushed notification).
  void Function(String peerId, String type, Map<String, dynamic> body)?
      onPush;

  String get ownPeerId     => _ownPeerId;
  String get publicAddress => _publicAddress;

  /// Internal hooks for PlayerServer so it can send binary chunks via the
  /// same FFI handle without us widening the public RatsClient surface.
  /// Not intended for general use.
  RatsBindings  get bindingsForServer => _b;
  Pointer<Void> get handleForServer   => _handle;

  /// Number of currently-connected librats peers (validated + raw).
  int get peerCount => _started ? _b.getPeerCount(_handle) : 0;

  /// Validated peer ids currently connected, in arbitrary order.
  List<String> get validatedPeerIds {
    if (!_started) return const [];
    final countPtr = malloc<Int32>();
    try {
      final arr = _b.getValidatedPeerIds(_handle, countPtr);
      final n = countPtr.value;
      final out = <String>[];
      if (arr.address == 0 || n <= 0) return out;
      for (int i = 0; i < n; ++i) {
        final p = arr.elementAt(i).value;
        if (p.address != 0) {
          out.add(p.toDartString());
          _b.stringFree(p);
        }
      }
      // Free the outer `char**` librats heap-allocated. rats_string_free
      // is just free() under the hood and accepts any malloc'd pointer
      // type; casting through Utf8* keeps the FFI signatures happy.
      // Without this we leak one machine word per call, which on a
      // busy session (every RPC routing check + 3-second watchdog) is
      // ~8 KB/min of accumulating heap.
      _b.stringFree(arr.cast<Utf8>());
      return out;
    } finally {
      malloc.free(countPtr);
    }
  }

  /// Direct rats_connect by host:port. Returns 0 on success.
  int connect(String host, int port) {
    final hostPtr = host.toNativeUtf8();
    try {
      return _b.connect(_handle, hostPtr, port);
    } finally {
      malloc.free(hostPtr);
    }
  }

  /// Drop the open librats peer with `peerId`. No-op if the peer is not
  /// connected. Returns 0 on success.
  int disconnectPeer(String peerId) {
    if (!_started || peerId.isEmpty) return -1;
    final pidPtr = peerId.toNativeUtf8();
    try {
      return _b.disconnectPeerById(_handle, pidPtr);
    } finally {
      malloc.free(pidPtr);
    }
  }

  /// Drop every currently-connected librats peer. Returns the number of
  /// peers we asked librats to disconnect. Triggered by the user
  /// long-pressing the Connect button — fully voluntary "go offline"
  /// path. The next forceReconnect() (tap on Connect) re-dials the VPS
  /// and rebuilds discovery state.
  int disconnectAll() {
    if (!_started) return 0;
    final ids = validatedPeerIds;
    for (final id in ids) {
      disconnectPeer(id);
    }
    return ids.length;
  }

  // -- Lifecycle ---------------------------------------------------------

  Future<void> _start() async {
    if (_started) return;

    final rc = _b.start(_handle);
    if (rc != 0) {
      throw StateError('rats_start failed (rc=$rc)');
    }

    final idPtr = _b.getOurPeerId(_handle);
    if (idPtr.address != 0) {
      _ownPeerId = idPtr.toDartString();
      _b.stringFree(idPtr);
    }

    // Hook up the global response receiver. Every API verb arrives on the
    // same channel so we register one listener and demux on `req_id`.
    _registerReplyHandler();

    // Register the binary callback so audio chunks delivered by full nodes
    // are routed to the in-flight _streams demuxer.
    _registerBinaryHandler();

    // Load the known mini-node list. The first time the player ever runs
    // there's nothing in prefs and we fall back to the hardcoded seed; on
    // subsequent runs the list contains whatever the previous run learned
    // from `mininodes.list` so we mesh against the full set even if the
    // original seed is offline.
    await _loadKnownMiniNodes();

    // Dial every known mini-node — librats's connect is async and de-dupes
    // duplicates internally so spamming a few at once is cheap. Multiple
    // connections means a single VPS going down doesn't strand us.
    for (final v in _knownMiniNodes) {
      _dialOne(v);
    }

    _started = true;

    // Spin up the BitTorrent-compatible DHT on a dedicated UDP port so
    // peers can find each other for a content_hash without going through
    // the VPS's in-memory swarm index. Failure here is non-fatal — we
    // still have the VPS-relayed swarm.locate as a fallback discovery
    // mechanism. The DHT bootstraps from public BitTorrent nodes baked
    // into librats and from any peer the rats client already knows.
    try {
      final dhtRc = _b.startDhtDiscovery(_handle, kDhtPort);
      if (dhtRc != 0) {
        // ignore: avoid_print
        print('[rats] start_dht_discovery rc=$dhtRc — DHT unavailable, '
              'swarm discovery will rely on the VPS relay');
      } else {
        // ignore: avoid_print
        print('[rats] DHT discovery started on UDP $kDhtPort');
      }
    } catch (e) {
      // ignore: avoid_print
      print('[rats] DHT init threw: $e');
    }

    // Once a handshake lands, query the mesh-discovery RPC so the list
    // grows beyond the hardcoded seed. We poll every 60 s to pick up
    // newly-spawned mini-nodes too.
    unawaited(_refreshMiniNodes());
    _miniNodeTimer = Timer.periodic(
        const Duration(minutes: 1), (_) => _refreshMiniNodes());

    // Ask the VPS what IP:port it sees us from via the mini-node's
    // `stun.observe` verb. We could use librats' native STUN client too,
    // but the VPS answer reflects the exact NAT mapping in use on the
    // rats peer connection — handy for surfacing direct/relay reachability
    // hints to the UI. Fire-and-forget; if it fails we just stay on relay.
    unawaited(_observePublicAddressViaVps());

    // Announce ourselves to the VPS player registry. Other players that
    // resolve our peer id through `swarm.locate`/`player.locate` will get
    // back the public_address recorded here, enabling a direct rats_connect
    // attempt before falling back to VPS relay (downloadFromSwarm).
    unawaited(_announceToVps());

    // Watchdog: re-dial the VPS if the rats handshake drops (the VPS
    // mini-node was restarted, the player's network changed, etc.).
    // Without this the player sits dead until the user kills + relaunches.
    // We also fire `onVpsReconnected` so callers (LibraryScanner) can
    // re-announce swarm membership the full node's in-memory map forgot
    // about across its restart.
    // 3-second tick so a dropped connection recovers in seconds rather
    // than waiting up to 10 seconds before the next dial attempt. The
    // librats dial is itself idempotent in async mode, so re-dialing
    // while already connecting is cheap.
    _reconnectTimer = Timer.periodic(const Duration(seconds: 3),
                                     (_) => _vpsWatchdog());

    // Wire the Android-side ConnectivityManager.NetworkCallback bridge.
    // When wifi ↔ cellular flips, the kernel will eventually surface the
    // dead TCP socket via keepalive probes (~30 s), but the platform
    // already knows the network changed — relaying that signal here cuts
    // the choppy gap down to a single redial round-trip.
    _wireNetworkChangeListener();
  }

  void _wireNetworkChangeListener() {
    // Channel only exists on Android (MainActivity registers it); on
    // Windows / desktop the setMethodCallHandler is a no-op since no
    // sender ever fires.
    const channel = MethodChannel('musicchain/network');
    channel.setMethodCallHandler((call) async {
      if (call.method == 'networkChanged') {
        forceVpsReconnect();
      }
      return null;
    });
  }

  /// Force an immediate redial of every known mini-node, bypassing the next
  /// 3-second watchdog tick. Called when an external signal (Android
  /// network change, user-initiated reconnect button) tells us the path
  /// to the VPS is stale.
  ///
  /// Idempotent: librats's connect is async and dedupes against in-flight
  /// connect attempts, so spamming this is safe. Combined with the
  /// duplicate-peer eviction in the patched librats (see
  /// project_librats_duplicate_peer_storm), the fresh socket wins over
  /// any half-dead one still mapped server-side.
  void forceVpsReconnect() {
    if (!_started) return;
    for (final v in _knownMiniNodes) {
      _dialOne(v);
    }
    _vpsWasUp = false;
  }

  /// Set of mini-nodes the player will dial at startup / on reconnect.
  /// Seeded from prefs (so previously-discovered VPSes survive an app
  /// restart) and from the hardcoded fallback when prefs are empty.
  final Set<MiniNodeAddr> _knownMiniNodes = {
    const MiniNodeAddr(kVpsHost, kVpsRatsPort),
  };
  Timer? _miniNodeTimer;

  /// Snapshot of the active mini-node set. UI surfaces use this to show
  /// "connected to 2 mini-nodes" instead of treating any single one as
  /// authoritative.
  List<MiniNodeAddr> get knownMiniNodes =>
      List.unmodifiable(_knownMiniNodes);

  void _dialOne(MiniNodeAddr v) {
    final hostPtr = v.host.toNativeUtf8();
    _b.connect(_handle, hostPtr, v.port);
    malloc.free(hostPtr);
  }

  Future<void> _loadKnownMiniNodes() async {
    try {
      final prefs = await SharedPreferences.getInstance();
      final raw = prefs.getStringList('known_mini_nodes') ?? const <String>[];
      for (final entry in raw) {
        final colon = entry.lastIndexOf(':');
        if (colon <= 0) continue;
        final host = entry.substring(0, colon);
        final port = int.tryParse(entry.substring(colon + 1));
        if (port == null || port <= 0 || port > 65535) continue;
        _knownMiniNodes.add(MiniNodeAddr(host, port));
      }
    } catch (_) { /* prefs unavailable on first run — fall back to seed */ }
  }

  Future<void> _saveKnownMiniNodes() async {
    try {
      final prefs = await SharedPreferences.getInstance();
      final list = _knownMiniNodes.map((v) => '${v.host}:${v.port}').toList();
      await prefs.setStringList('known_mini_nodes', list);
    } catch (_) {/* best effort */}
  }

  /// Ask every connected mini-node for its `mininodes.list` and merge
  /// the response into [_knownMiniNodes]. New entries are dialed on the
  /// spot so the player meshes against every reachable VPS, not just
  /// the one it bootstrapped through.
  Future<void> _refreshMiniNodes() async {
    final ids = validatedPeerIds;
    if (ids.isEmpty) return;
    final discovered = <MiniNodeAddr>{};
    for (final pid in ids) {
      try {
        final reply = await request(pid, 'mininodes.list', const {},
            timeout: const Duration(seconds: 6));
        if (reply is! List) continue;
        for (final e in reply) {
          if (e is! Map) continue;
          final addr = (e['public_address'] as String? ?? '').trim();
          if (addr.isEmpty) continue;
          final colon = addr.lastIndexOf(':');
          if (colon <= 0) continue;
          final host = addr.substring(0, colon);
          final port = int.tryParse(addr.substring(colon + 1));
          if (port == null || port <= 0 || port > 65535) continue;
          discovered.add(MiniNodeAddr(host, port));
        }
      } catch (_) { /* peer didn't implement mininodes.list — skip */ }
    }
    bool changed = false;
    for (final v in discovered) {
      if (_knownMiniNodes.add(v)) {
        changed = true;
        _dialOne(v); // wire up immediately so the mesh fills in
      }
    }
    if (changed) {
      unawaited(_saveKnownMiniNodes());
    }
  }

  /// Notified when the VPS handshake transitions empty → non-empty.
  /// LibraryScanner uses this to re-submit fingerprints so the full node's
  /// in-memory swarm map gets repopulated after it restarts.
  void Function()? onVpsReconnected;

  Timer? _reconnectTimer;
  bool   _vpsWasUp = false;

  void _vpsWatchdog() {
    final up = validatedPeerIds.isNotEmpty;
    if (!up) {
      // No live handshake — dial every known mini-node. rats_connect is
      // idempotent in async mode; a duplicate call when we're already
      // connecting is a no-op, so dialing the full set every tick is
      // cheap and gives whichever VPS comes back first the win.
      for (final v in _knownMiniNodes) {
        _dialOne(v);
      }
      _vpsWasUp = false;
      return;
    }
    if (!_vpsWasUp) {
      _vpsWasUp = true;
      // Re-announce ourselves + retry the public-address probe so other
      // peers can locate us, and tell upper layers to re-join swarms.
      unawaited(_announceToVps());
      unawaited(_observePublicAddressViaVps());
      try { onVpsReconnected?.call(); } catch (_) {}
    }
  }

  Timer? _announceTimer;

  Future<void> _announceToVps() async {
    for (int i = 0; i < 30; ++i) {
      if (!_started)                  return; // disposed mid-wait
      if (validatedPeerIds.isNotEmpty) break;
      await Future.delayed(const Duration(seconds: 1));
    }
    if (!_started)                   return;
    if (validatedPeerIds.isEmpty)    return;
    Future<void> announceOnce() async {
      final ids = validatedPeerIds;
      if (ids.isEmpty) return;
      try {
        await request(ids.first, 'player.announce',
            {'peer_id': _ownPeerId},
            timeout: const Duration(seconds: 6));
      } catch (_) {
        // VPS still warming up or transient relay drop — next tick will retry.
      }
    }
    await announceOnce();
    // The VPS tracks announced_at_ms so other players can age stale
    // entries; re-announcing every 5 minutes keeps our row fresh.
    // Reset the timer so repeated _announceToVps calls (e.g. on VPS
    // reconnect) don't leak overlapping periodics.
    _announceTimer?.cancel();
    _announceTimer = Timer.periodic(const Duration(minutes: 5),
                                    (_) => announceOnce());
  }

  Future<void> _observePublicAddressViaVps() async {
    // Wait for the VPS rats handshake.
    for (int i = 0; i < 30; ++i) {
      if (!_started)                   return;
      if (validatedPeerIds.isNotEmpty) break;
      await Future.delayed(const Duration(seconds: 1));
    }
    if (!_started)                return;
    if (validatedPeerIds.isEmpty) return;
    // stun.observe is mini-node-only; full nodes return unknown_type
    // post-pivot. Wait briefly for a mini-node to identify itself if
    // none has yet — the watchdog calls this on every reconnect so a
    // delayed mini.hello recovers on the next tick anyway.
    final vps = firstMiniNodePeerId ?? validatedPeerIds.first;
    try {
      final reply = await request(vps, 'stun.observe', const {},
          timeout: const Duration(seconds: 6));
      final addr = ((reply as Map?) ?? const {})['observed_address'] as String? ?? '';
      if (addr.isNotEmpty) _publicAddress = addr;
    } catch (_) {
      // Ignore — public_address remains empty and routing falls back to
      // VPS-relay for traffic that needs to reach this peer.
    }
  }

  // -- DHT swarm discovery ---------------------------------------------

  bool get isDhtRunning => _b.isDhtRunning(_handle) != 0;
  int  get dhtRoutingTableSize => _b.dhtRoutingTableSize(_handle);

  /// Tell the DHT we hold [sha1Hex] so other peers can find us. Pure
  /// announce — librats sees the null callback and skips the peer
  /// delivery path entirely.
  void announceOnly(String sha1Hex) {
    if (sha1Hex.length != 40) return;
    final hashPtr = sha1Hex.toNativeUtf8();
    try {
      _b.announceForHash(_handle, hashPtr, 0,
          Pointer<NativeFunction<NativePeersFoundCb>>.fromAddress(0),
          nullptr);
    } finally {
      malloc.free(hashPtr);
    }
  }

  // ---- DHT peer-discovery (musicchain librats patch) ------------------
  //
  // Upstream librats invoked the peers-found callback with transient
  // `const char**` pointers backed by a stack-local vector. Dart's
  // NativeCallable.listener runs the body asynchronously after the
  // lambda has returned, so reading the pointers was a textbook
  // use-after-free (every prior 0xc0000005 player crash). The
  // musicchain librats patch flips the contract: each address AND the
  // outer array are strdup'd onto the heap before the callback fires
  // and the consumer is responsible for freeing them.
  //
  // The Dart side keeps ONE long-lived NativeCallable per dht key
  // (callbacks must outlive the underlying announce — librats keeps
  // delivering peer batches over a multi-second window), copies the
  // strings into a Dart set, and immediately frees every pointer + the
  // array. The cb itself is registered in [_dhtCallbacks] and never
  // closed.

  /// LRU-bounded registry of per-hash DHT trampolines + result sinks.
  /// The map keeps insertion order; when we hit the cap we close the
  /// oldest callable + drop its results set. 512 unique hashes is well
  /// above any realistic working set (a download of song X queries
  /// that hash exactly once, then PieceDownloader reads from the
  /// cached set) — anything bigger would just pile up trampolines
  /// librats's DHT will never deliver new peers for.
  static const _kDhtRegistryCap = 512;
  final Map<String, NativeCallable<NativePeersFoundCb>> _dhtCallbacks = {};
  final Map<String, Set<String>>                       _dhtResults   = {};

  void _capDhtRegistry() {
    while (_dhtCallbacks.length > _kDhtRegistryCap) {
      final oldest = _dhtCallbacks.keys.first;
      try { _dhtCallbacks[oldest]?.close(); } catch (_) {}
      _dhtCallbacks.remove(oldest);
      _dhtResults.remove(oldest);
    }
  }

  NativeCallable<NativePeersFoundCb> _ensureDhtCallback(String sha1Hex) {
    final existing = _dhtCallbacks[sha1Hex];
    if (existing != null) return existing;
    final sink = _dhtResults.putIfAbsent(sha1Hex, () => <String>{});
    final cb = NativeCallable<NativePeersFoundCb>.listener(
      (Pointer<Void> _, Pointer<Pointer<Utf8>> arr, int count) {
        if (count <= 0 || arr.address == 0) {
          if (arr.address != 0) _freeOwnedArray(arr, count);
          return;
        }
        try {
          for (int i = 0; i < count; ++i) {
            final p = arr[i];
            if (p.address == 0) continue;
            String? s;
            try {
              s = p.toDartString();
            } catch (_) {
              s = null;
            }
            if (s != null && s.isNotEmpty && sink.length < 128) {
              sink.add(s);
            }
            _b.stringFree(p); // free per librats contract
          }
        } finally {
          // Always free the outer array, even if we threw mid-copy.
          // Bypasses the per-entry loop's `_b.stringFree(arr[i])` for
          // already-freed entries — we don't double-free because the
          // array itself is a separate malloc'd chunk.
          _freeOwnedArray(arr, 0); // entries already freed in the loop
        }
      },
    );
    _dhtCallbacks[sha1Hex] = cb;
    _capDhtRegistry();
    return cb;
  }

  /// Cast and free the array of strdup'd C strings librats handed us.
  /// `arr` is a heap-allocated `const char**` from librats's
  /// `std::malloc`. After [skipFirst] (which always = 0 in current
  /// callers) entries have already been individually freed by the
  /// caller. We only free the outer array here.
  void _freeOwnedArray(Pointer<Pointer<Utf8>> arr, int skipFirst) {
    // ignored param kept for future extension; today we always pass 0.
    // ignore: unused_local_variable
    final _ = skipFirst;
    // rats_string_free is just free() under the hood and ignores type,
    // so casting the array pointer to Utf8* and calling stringFree on
    // it correctly frees the malloc'd outer block.
    _b.stringFree(arr.cast<Utf8>());
  }

  /// Initiate (or refresh) a DHT search for [sha1Hex] and wait
  /// [listenWindow] for the first batches to land. Late-arriving
  /// peers continue to accumulate into the registry and a subsequent
  /// call sees them. Returns a snapshot of every peer the DHT has told
  /// us about for this hash.
  Future<List<String>> findHashHolders(
      String sha1Hex, {
      Duration listenWindow = const Duration(seconds: 6),
      }) async {
    if (sha1Hex.length != 40) {
      throw ArgumentError(
          'findHashHolders needs a 40-hex DHT key, got ${sha1Hex.length}');
    }
    final cb = _ensureDhtCallback(sha1Hex);
    final hashPtr = sha1Hex.toNativeUtf8();
    try {
      _b.announceForHash(_handle, hashPtr, 0, cb.nativeFunction, nullptr);
    } finally {
      malloc.free(hashPtr);
    }
    await Future<void>.delayed(listenWindow);
    final results = _dhtResults[sha1Hex];
    return results == null ? const <String>[] : results.toList(growable: false);
  }

  /// Tear down. Currently only used by tests. Closes every
  /// NativeCallable trampoline, cancels every pending timer, and lets
  /// the FFI handle go.
  void dispose() {
    if (!_started) return;
    _reconnectTimer?.cancel(); _reconnectTimer = null;
    _announceTimer?.cancel();  _announceTimer  = null;
    _miniNodeTimer?.cancel();  _miniNodeTimer  = null;
    // Cancel every queued early-chunk TTL timer the streaming receiver
    // installs. Without this, an interrupted stream's TTL keeps the
    // RatsClient pinned in memory after teardown.
    for (final t in _earlyChunkTtl.values) { t.cancel(); }
    _earlyChunkTtl.clear();
    for (final p in _pending.values) {
      p.timeout.cancel();
      if (!p.completer.isCompleted) {
        p.completer.completeError(StateError('client disposed'));
      }
    }
    _pending.clear();
    _streams.clear();
    // Close every persistent DHT NativeCallable. Closing forfeits the
    // safety invariant librats relies on (the trampoline outlives any
    // late-arriving peer batch), so we tear the rats client down FIRST
    // — `_b.stop` halts librats's worker threads, which guarantees the
    // DHT layer won't fire another callback after this point.
    _b.stop(_handle);
    for (final cb in _dhtCallbacks.values) {
      try { cb.close(); } catch (_) {}
    }
    _dhtCallbacks.clear();
    _dhtResults.clear();
    _b.destroy(_handle);
    _replyCallable.close();
    _requestCallable.close();
    _binaryCallable.close();
    if (identical(_activeForCallback, this)) {
      _activeForCallback = null;
    }
    _started = false;
  }

  // -- Request/response over librats typed messages ----------------------

  /// Send a request to `peerId` of the given `type` and await a JSON reply.
  ///
  /// The reply body can be any JSON value (Map, List, primitive). Callers that
  /// know the verb's shape can cast at the call site.
  ///
  /// Times out after [timeout]. Caller-visible errors are wrapped in
  /// [RatsRpcException].
  Future<Object?> request(
    String peerId,
    String type,
    Map<String, dynamic> body, {
    Duration timeout = const Duration(seconds: 15),
  }) async {
    if (!_started) {
      throw StateError('RatsClient not started');
    }

    // Cold-start race: when the player launches, the first songs.list
    // request fires within ~80 ms of RatsClient.start() — typically a
    // few ms BEFORE the VPS handshake validates. validatedPeerIds is
    // empty, the relay-fallback below has nothing to fall back to, and
    // the message is sent direct to a cached home-node peer id librats
    // has no socket for — internally rejected with "peer not found",
    // externally observed as a 15 s timeout. Wait briefly (up to ~3 s)
    // for the VPS to come up so the first request can route correctly
    // instead of burning the timeout.
    if (type != 'relay.forward' &&
        !validatedPeerIds.contains(peerId) &&
        validatedPeerIds.isEmpty) {
      final deadline = DateTime.now().add(const Duration(seconds: 3));
      while (validatedPeerIds.isEmpty && DateTime.now().isBefore(deadline)) {
        await Future<void>.delayed(const Duration(milliseconds: 100));
      }
    }

    // Routing decision for the outbound request:
    //
    //   1. If we already have an explicit relay mapping for this peer
    //      (LibratsDiscovery set one because reachability=relay), use it.
    //   2. Otherwise, if peerId is NOT a validated direct peer AND we
    //      have at least one validated peer (the VPS), fall back to
    //      relaying through that peer.
    //   3. Otherwise, send direct.
    final explicitRelay = _relayVia[peerId];
    String? routeVia;
    if (type != 'relay.forward') {
      if (explicitRelay != null && explicitRelay.isNotEmpty) {
        routeVia = explicitRelay;
      } else if (!validatedPeerIds.contains(peerId)) {
        // Prefer a peer we've confirmed is a mini-node (via mini.hello)
        // over an arbitrary librats validated peer. Pre-fix this used
        // `ids.first`, which after any peer-table churn (mini-node
        // restart, network blip) could be ANOTHER PLAYER — and players
        // don't handle relay.forward, so the relayed request bounced
        // back as "no handler for type=relay.forward". Mirrors the
        // pattern already used for stun.observe at line 531.
        routeVia = firstMiniNodePeerId
            ?? (validatedPeerIds.isNotEmpty ? validatedPeerIds.first : null);
      }
    }
    if (routeVia != null && routeVia != peerId) {
      return request(routeVia, 'relay.forward', {
        'target_peer_id': peerId,
        'type':           type,
        'body':           body,
      }, timeout: timeout);
    }

    // No route possible — target isn't validated and we have no VPS to
    // relay through. Surface this immediately as send_failed so the
    // caller's retry path (LibraryProvider._withRediscoverRetry) kicks
    // in instantly instead of waiting out the 15 s timeout.
    if (type != 'relay.forward' && !validatedPeerIds.contains(peerId)) {
      throw RatsRpcException(
        'send_failed',
        'no validated relay peer for $peerId');
    }

    final reqId = _newRequestId();
    final envelope = {
      'req_id': reqId,
      'type':   type,
      'body':   body,
    };

    final completer = Completer<Object?>();
    final t = Timer(timeout, () {
      final p = _pending.remove(reqId);
      if (p != null && !p.completer.isCompleted) {
        p.completer.completeError(
            RatsRpcException('timeout', 'no reply within ${timeout.inMilliseconds}ms'));
      }
    });
    _pending[reqId] = _PendingRequest(completer, t);

    // The librats wire-level message type is always "musicchain.request" —
    // the verb name lives inside the JSON envelope. The full node listens for
    // exactly that type and demuxes on `type` once it parses the body.
    final peerPtr = peerId.toNativeUtf8();
    final typePtr = 'musicchain.request'.toNativeUtf8();
    final bodyPtr = jsonEncode(envelope).toNativeUtf8();
    try {
      final rc = _b.sendMessage(_handle, peerPtr, typePtr, bodyPtr);
      if (rc != 0) {
        _pending.remove(reqId)?.timeout.cancel();
        return Future.error(RatsRpcException('send_failed', 'rats_send_message rc=$rc'));
      }
    } finally {
      malloc.free(peerPtr);
      malloc.free(typePtr);
      malloc.free(bodyPtr);
    }
    return completer.future;
  }

  // -- Reply demux -------------------------------------------------------

  /// Static dispatch slot the FFI message listener reads. Null until
  /// `_registerReplyHandler` runs; if a second RatsClient.initialize
  /// fires before the first instance is disposed, the second-init
  /// detector in `_registerReplyHandler` crashes loudly instead of
  /// silently stealing every reply from the first instance.
  static RatsClient? _activeForCallback;

  /// `NativeCallable.listener` lets librats invoke this from worker threads
  /// without violating Dart's isolate rules — the listener body runs on the
  /// owning isolate via a SendPort. We keep the callable as a field so it is
  /// not GC'd while librats still holds the function pointer.
  late final NativeCallable<NativeMessageCb> _replyCallable;

  /// Listener body. Args arrive as native pointers owned by librats; our
  /// patched librats strdup()'s them so they survive the async hop into the
  /// Dart isolate event loop, and we free both buffers after reading.
  static void _onMessageListener(
      Pointer<Void> _, Pointer<Utf8> peerIdPtr, Pointer<Utf8> dataPtr) {
    final self = _activeForCallback;
    if (self == null) {
      // FFI listener fired before initialize completed or after dispose.
      // Drop the message and free what librats handed us so we don't
      // leak the strdup'd args.
      if (peerIdPtr.address != 0) malloc.free(peerIdPtr);
      if (dataPtr.address   != 0) malloc.free(dataPtr);
      return;
    }
    final b = self._b;
    try {
      if (dataPtr.address == 0) return;
      String body;
      try {
        body = dataPtr.toDartString();
      } catch (_) {
        return; // unparseable / null deref protection
      }
      String peerId = '';
      if (peerIdPtr.address != 0) {
        try { peerId = peerIdPtr.toDartString(); } catch (_) {}
      }
      self._dispatchReply(peerId, body);
    } finally {
      if (peerIdPtr.address != 0) b.stringFree(peerIdPtr);
      if (dataPtr.address   != 0) b.stringFree(dataPtr);
    }
  }

  void _registerReplyHandler() {
    // The FFI listeners are static (NativeCallable signatures can't
    // close over instance state cleanly with .listener), so they have
    // to dispatch via this static slot. If we end up here twice (tests
    // re-initializing without disposing the first instance), the
    // second registration steals every reply from the first — which
    // is a silent correctness break. Crash loudly instead.
    final prior = _activeForCallback;
    if (prior != null && prior != this) {
      throw StateError(
        'RatsClient already initialized; dispose the previous instance '
        'before calling initialize() again');
    }
    _activeForCallback = this;
    _replyCallable =
        NativeCallable<NativeMessageCb>.listener(_onMessageListener);

    // Single message-type name for all replies. The full node sends every
    // RPC response under this name; payload contains req_id + status + body.
    final type = 'musicchain.reply'.toNativeUtf8();
    _b.onMessage(_handle, type, _replyCallable.nativeFunction, nullptr);
    malloc.free(type);

    // Players also accept incoming RPCs from other players (peer-to-peer
    // swarm streaming). The handler dispatches via [onRequest] and writes
    // a musicchain.reply with the same req_id back to the caller. Default
    // dispatch returns "unknown_type" so a missing onRequest doesn't break
    // anyone — the song-serving handler lives in the higher-level
    // PlayerServer (see services/player_server.dart).
    _requestCallable =
        NativeCallable<NativeMessageCb>.listener(_onRequestListener);
    final reqType = 'musicchain.request'.toNativeUtf8();
    _b.onMessage(_handle, reqType, _requestCallable.nativeFunction, nullptr);
    malloc.free(reqType);
  }

  late final NativeCallable<NativeMessageCb> _requestCallable;

  /// App-installed handler for incoming `musicchain.request` envelopes
  /// from other peers (other players, mini-nodes). Must return a JSON
  /// reply body to put into the `body` field of the outbound
  /// musicchain.reply, or `null` to signal an "unknown_type" response.
  ///
  /// * [peerId]    — the librats peer id of the immediate sender (which is
  ///                 the VPS mini-node when the request came in over relay).
  /// * [type]      — the inner verb (e.g. `stream.open`).
  /// * [body]      — the inner request body Map.
  /// * [originator] — non-empty when the VPS relayed this on behalf of
  ///                 another player; this is that player's peer id. The
  ///                 PlayerServer uses it to tag outbound chunks with the
  ///                 1-byte 'F' relay header + 40-byte target so the VPS
  ///                 strips and replays them. Empty for direct peer-to-peer.
  Future<Map<String, dynamic>?> Function(
      String peerId, String type, Map<String, dynamic> body,
      {String originator})? onRequest;

  static void _onRequestListener(
      Pointer<Void> _, Pointer<Utf8> peerIdPtr, Pointer<Utf8> dataPtr) {
    final self = _activeForCallback;
    if (self == null) {
      if (peerIdPtr.address != 0) malloc.free(peerIdPtr);
      if (dataPtr.address   != 0) malloc.free(dataPtr);
      return;
    }
    final b = self._b;
    String peerId = '';
    String body   = '';
    try {
      if (dataPtr.address == 0) return;
      try { body = dataPtr.toDartString(); } catch (_) { return; }
      if (peerIdPtr.address != 0) {
        try { peerId = peerIdPtr.toDartString(); } catch (_) {}
      }
    } finally {
      if (peerIdPtr.address != 0) b.stringFree(peerIdPtr);
      if (dataPtr.address   != 0) b.stringFree(dataPtr);
    }
    self._dispatchRequest(peerId, body);
  }

  void _dispatchRequest(String peerId, String raw) {
    Map<String, dynamic> env;
    try {
      env = jsonDecode(raw) as Map<String, dynamic>;
    } catch (_) { return; }
    final reqId      = env['req_id']            as String? ?? '';
    final type       = env['type']              as String? ?? '';
    final inner      = (env['body']             as Map<String, dynamic>?) ?? const {};
    // VPS-relayed envelopes carry the original player's peer id under
    // `originator_peer_id` (see tools/mini_node.cpp relay.forward).
    final originator = env['originator_peer_id'] as String? ?? '';
    if (reqId.isEmpty || type.isEmpty) return;

    // mini-node identity. The mini-node sends mini.hello to every
    // fresh peer; the player snapshots which connected peer is a
    // mini-node so requestRoutes / stun.observe can target it
    // directly instead of guessing via validatedPeerIds.first (which
    // is wrong any time librats also handed us a full-node DHT peer).
    if (type == 'mini.hello') {
      // Prune entries that librats no longer reports as validated before
      // recording the new one. Every reconnect of the same mini-node
      // hands us a fresh peer_id (librats peer ids are per-handshake),
      // so without this _miniNodePeerIds accumulates dead ids
      // indefinitely over a long session (VPS restarts, network flaps).
      // firstMiniNodePeerId already filters via validatedPeerIds so the
      // public API stays correct, but the set itself would otherwise
      // grow without bound.
      final live = validatedPeerIds.toSet();
      _miniNodePeerIds.removeWhere((id) => !live.contains(id));
      final fresh = _miniNodePeerIds.add(peerId);
      if (fresh) {
        // ignore: avoid_print
        print('[rats] mini.hello from ${peerId.substring(0, peerId.length < 12 ? peerId.length : 12)}'
              '… (wallet=${(inner['wallet'] as String? ?? '').isNotEmpty})');
      }
      // Ack so the mini-node logs a clean request/reply pair.
      final ack = jsonEncode({
        'req_id': reqId,
        'status': 'ok',
        'body':   const <String, dynamic>{},
      });
      final peerPtr = peerId.toNativeUtf8();
      final typePtr = 'musicchain.reply'.toNativeUtf8();
      final bodyPtr = ack.toNativeUtf8();
      try {
        _b.sendMessage(_handle, peerPtr, typePtr, bodyPtr);
      } finally {
        malloc.free(peerPtr);
        malloc.free(typePtr);
        malloc.free(bodyPtr);
      }
      return;
    }

    Future<void> answer() async {
      Map<String, dynamic>? body;
      String status = 'ok';
      String? error;
      try {
        final cb = onRequest;
        body = cb == null
            ? null
            : await cb(peerId, type, inner, originator: originator);
        if (body == null) {
          status = 'unknown_type';
          error  = 'no handler for type=$type';
        }
      } catch (e) {
        status = 'server_error';
        error  = e.toString();
      }
      final reply = <String, dynamic>{
        'req_id': reqId,
        'status': status,
        if (body != null) 'body': body,
        if (error != null) 'error': error,
      };
      final peerPtr = peerId.toNativeUtf8();
      final typePtr = 'musicchain.reply'.toNativeUtf8();
      final bodyPtr = jsonEncode(reply).toNativeUtf8();
      try {
        _b.sendMessage(_handle, peerPtr, typePtr, bodyPtr);
      } finally {
        malloc.free(peerPtr);
        malloc.free(typePtr);
        malloc.free(bodyPtr);
      }
    }

    unawaited(answer());
  }

  late final NativeCallable<NativeBinaryCb> _binaryCallable;

  /// Static trampoline for binary deliveries. Patched librats hands us
  /// strdup'd peer_id + malloc'd data; we copy out into Dart-owned memory
  /// and free both buffers before returning.
  static void _onBinaryListener(
      Pointer<Void> _, Pointer<Utf8> peerIdPtr,
      Pointer<Void> dataPtr, int size) {
    final self = _activeForCallback;
    if (self == null) {
      // FFI listener fired before initialize completed or after dispose.
      // Free BOTH buffers librats handed us — patched librats malloc()s
      // the binary data buffer too (see normal path below), so leaving
      // it for "librats itself" leaks the chunk (up to 1 MB per fire).
      // rats_string_free is just free() under the hood and ignores type,
      // so plain malloc.free on the cast pointer is equivalent.
      if (peerIdPtr.address != 0) malloc.free(peerIdPtr);
      if (dataPtr.address   != 0) malloc.free(dataPtr.cast<Utf8>());
      return;
    }
    final b = self._b;
    try {
      if (size < 9 || dataPtr.address == 0) return;
      final bytes = dataPtr.cast<Uint8>().asTypedList(size);
      final copy  = Uint8List.fromList(bytes);
      self._dispatchBinary(copy);
    } finally {
      if (peerIdPtr.address != 0) b.stringFree(peerIdPtr);
      // librats malloc()s the binary buffer — free with the same allocator.
      if (dataPtr.address != 0) {
        b.stringFree(dataPtr.cast<Utf8>());
      }
    }
  }

  void _registerBinaryHandler() {
    _binaryCallable =
        NativeCallable<NativeBinaryCb>.listener(_onBinaryListener);
    _b.setBinaryCb(_handle, _binaryCallable.nativeFunction, nullptr);
  }

  /// Buffer for chunks that arrive BEFORE the requester has registered its
  /// receiver. The server starts pushing chunks the moment it returns the
  /// reply to `stream.open`, so it is possible for the first one or two
  /// chunks to land on the binary callback before the requester's
  /// `requestAudio` future has resumed and run `_streams[streamId] = r;`.
  /// Without buffering those land in the `r == null` branch below and are
  /// dropped, leaving the receiver hung forever on a missing seq=0.
  final Map<int, List<Uint8List>> _earlyChunks = {};
  final Map<int, Timer>           _earlyChunkTtl = {};

  /// Move any buffered early chunks into the freshly-registered receiver
  /// (called from `requestAudio` / `downloadFromSwarm` after
  /// `_streams[streamId] = receiver`).
  void _drainEarlyChunks(int streamId, _AudioReceiver receiver) {
    final pending = _earlyChunks.remove(streamId);
    _earlyChunkTtl.remove(streamId)?.cancel();
    if (pending == null) return;
    for (final bytes in pending) {
      final seq = bytes[4]
          | (bytes[5] << 8)
          | (bytes[6] << 16)
          | (bytes[7] << 24);
      final eof = bytes[8] != 0;
      receiver.feed(seq, Uint8List.fromList(bytes.sublist(9)), eof);
      if (eof) _streams.remove(streamId);
    }
  }

  /// Hard ceiling on any single audio chunk we'll accept from a peer.
  /// Streams chunk at 16 KB on the server (`PlayerServer.chunkPayload`),
  /// and the legacy full-node streamer caps at 32 KB. 1 MB covers any
  /// historical variant generously while still rejecting frames a
  /// misbehaving / malicious peer might use to spike Dart heap.
  static const int _kMaxBinaryChunk = 1 * 1024 * 1024;

  void _dispatchBinary(Uint8List bytes) {
    if (bytes.length < 9) return;
    if (bytes.length > _kMaxBinaryChunk) return;
    final streamId = bytes[0]
        | (bytes[1] << 8)
        | (bytes[2] << 16)
        | (bytes[3] << 24);
    final seq = bytes[4]
        | (bytes[5] << 8)
        | (bytes[6] << 16)
        | (bytes[7] << 24);
    final eof = bytes[8] != 0;
    // View over the same backing buffer instead of `sublist` which
    // allocates a fresh Uint8List + memcpy on every chunk. On a hot
    // stream that's 1 alloc + N bytes copied every 16 KB; the view
    // is O(1).
    final payload = Uint8List.view(
        bytes.buffer, bytes.offsetInBytes + 9, bytes.length - 9);

    final r = _streams[streamId];
    if (r == null) {
      // Could be a late chunk after stream completion / cancel — OR
      // could be a chunk that arrived in the race window between the
      // server returning `stream.open` and the requester's
      // `_streams[id] = r;` landing. Park it in `_earlyChunks` for up
      // to 5s so the requester can pick it up in `_drainEarlyChunks`;
      // after that, drop it. Caps on both the number of distinct
      // stream ids buffered (defends against a peer spamming
      // unfulfilled streamIds) and the per-stream chunk list (a
      // single peer can't OOM us by spraying chunks at one streamId).
      if (_earlyChunks.length >= 64 &&
          !_earlyChunks.containsKey(streamId)) {
        return; // map is full; drop unknown-stream chunks silently
      }
      final pending =
          _earlyChunks.putIfAbsent(streamId, () => <Uint8List>[]);
      if (pending.length < 32) pending.add(bytes);
      _earlyChunkTtl.putIfAbsent(streamId, () => Timer(
        const Duration(seconds: 5),
        () {
          _earlyChunks.remove(streamId);
          _earlyChunkTtl.remove(streamId);
        },
      ));
      return;
    }
    r.feed(seq, payload, eof);
    if (eof) _streams.remove(streamId);
  }

  /// Pull the current routing table from the VPS mini-node by sending
  /// a `routes.get` RPC. Targets a peer that self-identified as a
  /// mini-node via mini.hello — and ONLY that peer. The previous
  /// fallback to validatedPeerIds.first lit up "no handler for
  /// type=routes.get" errors any time librats had already peer-
  /// exchanged a non-mini-node peer (another player, a full node)
  /// into the validated set before mini.hello landed, because that
  /// peer would reject the request as unknown.
  ///
  /// On cold start the mini-node's hello arrives ~500 ms after the
  /// TCP handshake, so we wait up to 4 s for it to register before
  /// returning empty. The next periodic refresh will retry without
  /// the wait.
  Future<List<Map<String, dynamic>>> requestRoutes(
      {Duration timeout = const Duration(seconds: 6)}) async {
    String? mini = firstMiniNodePeerId;
    if (mini == null) {
      for (int i = 0; i < 8 && mini == null; ++i) {
        await Future.delayed(const Duration(milliseconds: 500));
        mini = firstMiniNodePeerId;
      }
    }
    if (mini == null) {
      // ignore: avoid_print
      print('[rats] requestRoutes: no mini-node identified — '
            'validatedPeers=${validatedPeerIds.length}');
      return const [];
    }
    final body = await request(mini, 'routes.get', const {},
                                timeout: timeout);
    final m = (body as Map<String, dynamic>?) ?? const {};
    final peers = (m['peers'] as List?) ?? const [];
    return peers
        .whereType<Map>()
        .map((e) => Map<String, dynamic>.from(e))
        .toList();
  }

  // uploadAudio() removed: under the post-pivot architecture the player
  // never uploads bytes to a full node. Songs enter the chain via
  // fingerprint.submit (LibraryScanner) and bytes flow peer-to-peer via
  // PlayerServer.stream.open + downloadFromSwarm.

  /// Discover the swarm for `contentHash` via `nodePeerId`, then fetch
  /// the bytes from one of the swarm members. Falls back through:
  ///   1. direct rats_connect to the member's public address (ICE-equivalent)
  ///   2. VPS relay (setRelayVia) if direct doesn't establish
  ///   3. next swarm member if both fail
  ///
  /// Returns the fully reassembled audio bytes or throws on exhaustion.
  Future<List<int>> downloadFromSwarm({
    required String nodePeerId,
    required String contentHash,
    String? vpsPeerId,
    Duration totalTimeout = const Duration(minutes: 5),
    int?     preferredBitrate,
    /// Fires on every chunk so callers (DownloadProvider) can update a
    /// progress meter without polling. `total` is 0 when the full node's
    /// stream.open reply didn't include a size — UI should treat that
    /// as indeterminate.
    void Function(int received, int total)? onProgress,
    /// If no chunk arrives for this long, abort the current peer's
    /// stream and fall through to the next swarm member. Without this
    /// a stalled TCP socket would burn the whole `totalTimeout`,
    /// which during an album batch download chains into one stuck song
    /// timing out the rest.
    Duration chunkStall = const Duration(seconds: 30),
  }) async {
    // 1. Ask the full node who has the file.
    final probe = await request(nodePeerId, 'stream.open',
        {'content_hash': contentHash},
        timeout: const Duration(seconds: 8)) as Map<String, dynamic>;
    if (probe['source'] == 'local' || probe['stream_id'] != null) {
      // Full node has the bytes (legacy path; new arch should never hit
      // this but keep it as a fallback). Bytes are already coming.
      final streamId   = probe['stream_id']   as int;
      final totalBytes = probe['total_bytes'] as int;
      final receiver = _AudioReceiver(totalBytes);
      _streams[streamId] = receiver;
      // Single try/finally around setup AND wait: a throw in
      // _drainEarlyChunks or anywhere else between the map insert and
      // the await would otherwise leave the receiver pinned in
      // _streams forever.
      try {
        _drainEarlyChunks(streamId, receiver);
        if (onProgress != null) receiver.onProgress(onProgress);
        return await _awaitWithStallGuard(
            receiver, totalTimeout, chunkStall);
      } finally {
        _streams.remove(streamId);
      }
    }
    final variants = _parseVariants(probe['peers']);
    if (variants.isEmpty) {
      throw RatsRpcException('no_swarm',
          'no swarm peers for $contentHash');
    }
    final ordered = _pickOrder(variants, preferredBitrate: preferredBitrate);

    // 2. Resolve public addresses via the VPS player.locate, if we have one.
    final addresses = await _resolveAddresses(
      ordered.map((v) => v.peerId).toSet().toList(),
      vpsPeerId: vpsPeerId,
    );

    // 3. Try each variant in preference order.
    Object? lastError;
    for (final v in ordered) {
      try {
        await _maybeDirectConnect(v.peerId, addresses[v.peerId], vpsPeerId);
        final bytes = await requestAudio(v.peerId, v.contentHash,
            openTimeout:  const Duration(seconds: 10),
            totalTimeout: totalTimeout,
            chunkStall:   chunkStall,
            onProgress:   onProgress);
        return bytes;
      } catch (e) {
        lastError = e;
        // Try next swarm member.
      }
    }
    throw RatsRpcException('swarm_exhausted',
        'no swarm member served $contentHash (last error: $lastError)');
  }

  /// Wait for the receiver's bytes, but fail fast if a chunk hasn't
  /// landed in `chunkStall` (default 30 s). The receiver's `lastChunkAt`
  /// is updated on every feed; we poll it at half the stall interval.
  ///
  /// Returns the assembled bytes on success. Throws TimeoutException on
  /// total-timeout, RatsRpcException('chunk_stalled', …) on stall.
  Future<List<int>> _awaitWithStallGuard(
      _AudioReceiver receiver,
      Duration totalTimeout,
      Duration chunkStall) async {
    final deadline = DateTime.now().add(totalTimeout);
    final pollEvery = Duration(
        milliseconds: max(500, chunkStall.inMilliseconds ~/ 2));
    while (true) {
      final remaining = deadline.difference(DateTime.now());
      if (remaining.isNegative) {
        receiver.cancel();
        throw TimeoutException('download exceeded $totalTimeout');
      }
      final wait = remaining < pollEvery ? remaining : pollEvery;
      try {
        return await receiver.future.timeout(wait);
      } on TimeoutException {
        final since = DateTime.now().difference(receiver.lastChunkAt);
        if (since >= chunkStall) {
          receiver.cancel();
          throw RatsRpcException('chunk_stalled',
              'no chunk received for $since (stall limit $chunkStall)');
        }
        // Still within the chunk window — keep waiting.
      }
    }
  }

  /// Parse the variant-aware peers list returned by the full node's
  /// stream.open. Each element is either an object (new format) or a
  /// bare peer-id string (legacy clients before the variant rework).
  /// Defaults fill in for the legacy shape so existing peers still
  /// participate while everyone upgrades.
  List<SwarmVariant> _parseVariants(dynamic raw) {
    final out = <SwarmVariant>[];
    if (raw is! List) return out;
    for (final e in raw) {
      if (e is String) {
        out.add(SwarmVariant(
          peerId:      e,
          contentHash: '',
          bitrate:     0,
          audioFormat: 'mp3',
        ));
      } else if (e is Map) {
        final m = e.cast<String, dynamic>();
        out.add(SwarmVariant(
          peerId:      m['peer_id']      as String? ?? '',
          contentHash: m['content_hash'] as String? ?? '',
          bitrate:     m['bitrate']      as int?    ?? 0,
          audioFormat: m['audio_format'] as String? ?? 'mp3',
        ));
      }
    }
    return out.where((v) => v.peerId.isNotEmpty).toList();
  }

  /// Order variants by preference. Without [preferredBitrate] we put the
  /// lowest bitrate first (streaming default — starts decoding sooner on
  /// cellular). With [preferredBitrate] set, we sort by absolute distance
  /// from that target so download picks closest-to-asked-quality.
  List<SwarmVariant> _pickOrder(
    List<SwarmVariant> variants, {
    int? preferredBitrate,
  }) {
    final sorted = [...variants];
    if (preferredBitrate == null) {
      sorted.sort((a, b) {
        // 0 (unknown) bitrate sorts last so we prefer known-low to unknown.
        if (a.bitrate == 0 && b.bitrate != 0) return 1;
        if (b.bitrate == 0 && a.bitrate != 0) return -1;
        return a.bitrate.compareTo(b.bitrate);
      });
    } else {
      sorted.sort((a, b) {
        if (a.bitrate == 0 && b.bitrate != 0) return 1;
        if (b.bitrate == 0 && a.bitrate != 0) return -1;
        return (a.bitrate - preferredBitrate)
            .abs()
            .compareTo((b.bitrate - preferredBitrate).abs());
      });
    }
    return sorted;
  }

  Future<Map<String, String>> _resolveAddresses(
    List<String> peerIds, {
    String? vpsPeerId,
  }) async {
    final addresses = <String, String>{};
    if (vpsPeerId == null || vpsPeerId.isEmpty || peerIds.isEmpty) {
      return addresses;
    }
    try {
      final loc = await request(vpsPeerId, 'player.locate',
          {'peer_ids': peerIds},
          timeout: const Duration(seconds: 5));
      if (loc is List) {
        for (final e in loc) {
          final m = (e as Map?)?.cast<String, dynamic>() ?? const {};
          final pid = m['peer_id']        as String? ?? '';
          final pub = m['public_address'] as String? ?? '';
          if (pid.isNotEmpty && pub.isNotEmpty) addresses[pid] = pub;
        }
      }
    } catch (_) {/* fall through to relay-only */}
    return addresses;
  }

  Future<void> _maybeDirectConnect(
    String peerId,
    String? addr,
    String? vpsPeerId,
  ) async {
    bool direct = false;
    if (addr != null) {
      // Use lastIndexOf so IPv6 literals like 2001:db8::1:8080 parse to
      // host=2001:db8::1 / port=8080 instead of host=2001 / port=garbage.
      // The mini-node list parsers (_loadKnownMiniNodes, _refreshMiniNodes)
      // already do this; the indexOf here silently dropped every IPv6
      // public_address into the relay fallback path.
      final colon = addr.lastIndexOf(':');
      if (colon > 0) {
        final host = addr.substring(0, colon);
        final port = int.tryParse(addr.substring(colon + 1)) ?? 0;
        if (port > 0) {
          connect(host, port);
          for (int i = 0; i < 20; ++i) {
            await Future.delayed(const Duration(milliseconds: 250));
            if (validatedPeerIds.contains(peerId)) { direct = true; break; }
          }
        }
      }
    }
    if (direct) {
      setRelayVia(peerId, null);
    } else if (vpsPeerId != null && vpsPeerId.isNotEmpty) {
      setRelayVia(peerId, vpsPeerId);
    }
  }

  /// Same shape as [downloadFromSwarm] but returns an [AudioStream] the
  /// caller can drain chunk-by-chunk as bytes land. Lets the player open
  /// media_kit on the first chunk instead of blocking on the full
  /// download — the difference between "music plays in 300 ms" and "music
  /// plays after 10 s of cellular pipe."
  Future<AudioStream> streamFromSwarm({
    required String nodePeerId,
    required String contentHash,
    String? vpsPeerId,
    int? preferredBitrate,
  }) async {
    final probe = await request(nodePeerId, 'stream.open',
        {'content_hash': contentHash},
        timeout: const Duration(seconds: 8)) as Map<String, dynamic>;
    if (probe['source'] == 'local' || probe['stream_id'] != null) {
      final streamId   = probe['stream_id']   as int;
      final totalBytes = probe['total_bytes'] as int;
      final receiver = _AudioReceiver(totalBytes);
      _streams[streamId] = receiver;
      _drainEarlyChunks(streamId, receiver);
      return AudioStream._(streamId, totalBytes, receiver, _streams);
    }
    final variants = _parseVariants(probe['peers']);
    if (variants.isEmpty) {
      throw RatsRpcException('no_swarm',
          'no swarm peers for $contentHash');
    }
    final ordered = _pickOrder(variants, preferredBitrate: preferredBitrate);
    final addresses = await _resolveAddresses(
      ordered.map((v) => v.peerId).toSet().toList(),
      vpsPeerId: vpsPeerId,
    );

    Object? lastError;
    final fetchHash = (SwarmVariant v) => v.contentHash.isNotEmpty
        ? v.contentHash
        : contentHash;
    for (final v in ordered) {
      await _maybeDirectConnect(v.peerId, addresses[v.peerId], vpsPeerId);
      // Retry once: the first stream.open against a freshly-relayed
      // peer often races the VPS forwarding table. A 300 ms wait +
      // single retry is enough to make playback start on the first
      // tap instead of "click play, get nothing, click play again."
      Map<String, dynamic>? reply;
      for (int attempt = 0; attempt < 2; ++attempt) {
        try {
          reply = await request(v.peerId, 'stream.open',
              {'content_hash': fetchHash(v)},
              timeout: const Duration(seconds: 8))
              as Map<String, dynamic>;
          break;
        } catch (e) {
          lastError = e;
          reply = null;
          if (attempt == 0) {
            await Future.delayed(const Duration(milliseconds: 300));
          }
        }
      }
      if (reply == null) continue;
      if (reply['matched'] == false) {
        lastError = RatsRpcException('not_matched',
            'peer ${v.peerId} no longer has ${fetchHash(v)}');
        continue;
      }
      final streamId   = reply['stream_id']   as int;
      final totalBytes = reply['total_bytes'] as int;
      final receiver = _AudioReceiver(totalBytes);
      _streams[streamId] = receiver;
      _drainEarlyChunks(streamId, receiver);
      return AudioStream._(streamId, totalBytes, receiver, _streams);
    }
    throw RatsRpcException('swarm_exhausted',
        'no swarm member served $contentHash (last error: $lastError)');
  }

  /// Probe the full node for the variant list backing [contentHash]
  /// without committing to a fetch. Used by the download UI to populate
  /// a quality picker — distinct (bitrate, audio_format) tuples each
  /// become one option. Returns an empty list if the song is unknown
  /// or the swarm is empty.
  Future<List<SwarmVariant>> lookupSwarmVariants({
    required String nodePeerId,
    required String contentHash,
  }) async {
    try {
      final probe = await request(nodePeerId, 'stream.open',
          {'content_hash': contentHash},
          timeout: const Duration(seconds: 6)) as Map<String, dynamic>;
      return _parseVariants(probe['peers']);
    } catch (_) {
      return const <SwarmVariant>[];
    }
  }

  /// Open an audio stream from `peerId` for `contentHash` and return a Future
  /// that resolves to the fully reassembled audio bytes once the peer
  /// finishes sending all chunks. The reply to `stream.open` carries the
  /// allocated stream_id; chunks arrive on the binary channel.
  Future<List<int>> requestAudio(
      String peerId, String contentHash,
      {Duration openTimeout  = const Duration(seconds: 8),
       Duration totalTimeout = const Duration(minutes: 5),
       Duration chunkStall   = const Duration(seconds: 30),
       void Function(int received, int total)? onProgress}) async {
    final reply = await request(peerId, 'stream.open',
        {'content_hash': contentHash}, timeout: openTimeout);
    final meta = reply as Map<String, dynamic>;
    if (meta['matched'] == false) {
      // Peer no longer has the bytes (entry was removed locally between
      // the full node's swarm reply and our follow-up). Bail so the
      // caller tries the next swarm member.
      throw RatsRpcException('not_matched',
          'peer $peerId no longer has $contentHash');
    }
    final streamId   = meta['stream_id']   as int;
    final totalBytes = meta['total_bytes'] as int;

    final receiver = _AudioReceiver(totalBytes);
    _streams[streamId] = receiver;
    if (onProgress != null) receiver.onProgress(onProgress);
    // Pick up any chunks that landed in the race window between the
    // server returning the reply and us reaching this line.
    _drainEarlyChunks(streamId, receiver);
    try {
      return await _awaitWithStallGuard(receiver, totalTimeout, chunkStall);
    } finally {
      _streams.remove(streamId);
    }
  }

  void _dispatchReply(String peerId, String raw) {
    Map<String, dynamic> j;
    try {
      j = jsonDecode(raw) as Map<String, dynamic>;
    } catch (_) {
      return;
    }
    final reqId = j['req_id'] is String ? j['req_id'] as String : null;
    if (reqId != null) {
      final p = _pending.remove(reqId);
      if (p != null) {
        p.timeout.cancel();
        if (!p.completer.isCompleted) {
          // Extract status/error/body defensively. A malformed reply
          // (e.g. `status` arrives as a JSON number) would otherwise
          // throw a TypeError between the `_pending.remove` above and
          // the `complete()`/`completeError()` below, orphaning the
          // completer — the timer has been cancelled and the entry
          // removed, so the caller awaiting `completer.future` would
          // hang forever. Type-check instead of casting.
          try {
            final rawStatus = j['status'];
            final status = rawStatus is String ? rawStatus : 'ok';
            if (status == 'ok') {
              p.completer.complete(j['body']); // may be Map / List / primitive
            } else {
              final rawErr = j['error'];
              final err = rawErr is String ? rawErr : 'server error';
              p.completer.completeError(RatsRpcException(status, err));
            }
          } catch (e, st) {
            // Any unexpected failure decoding the envelope must still
            // surface to the caller — never leave the completer dangling.
            p.completer.completeError(
                RatsRpcException('malformed_reply', 'reply decode failed: $e'),
                st);
          }
        }
        return;
      }
    }
    // Unsolicited push (notification, broadcast).
    final type = j['type'] as String? ?? '';
    final body = (j['body'] as Map<String, dynamic>?) ?? const {};
    onPush?.call(peerId, type, body);
  }

  String _newRequestId() {
    final r = Random.secure();
    final bytes = List<int>.generate(8, (_) => r.nextInt(256));
    return bytes.map((b) => b.toRadixString(16).padLeft(2, '0')).join();
  }

  // -- Native lib resolution --------------------------------------------

  static DynamicLibrary _loadLibrary() {
    // mc_rats.dll / libmc_rats.so is vanilla librats with a small Dart-async
    // compatibility patch (callback args are strdup'd so they survive the
    // hop through NativeCallable.listener). Wire protocol is plain librats
    // TCP — no QUIC, no msquic.
    if (Platform.isAndroid) {
      return DynamicLibrary.open('libmc_rats.so');
    } else if (Platform.isWindows) {
      return DynamicLibrary.open('mc_rats.dll');
    } else if (Platform.isLinux) {
      return DynamicLibrary.open('libmc_rats.so');
    } else if (Platform.isMacOS) {
      return DynamicLibrary.open('libmc_rats.dylib');
    }
    throw UnsupportedError('Unsupported platform: ${Platform.operatingSystem}');
  }
}

class RatsRpcException implements Exception {
  RatsRpcException(this.status, this.message);
  final String status;
  final String message;
  @override
  String toString() => 'RatsRpcException($status): $message';
}

/// Reassembles audio chunks for a single stream. Two flavors live on the
/// same buffer:
///   * `future` — completes once EOF arrives and every seq is in. Used by
///     callers that need the whole blob (legacy `requestAudio`).
///   * `stream` — emits in-order chunks as they arrive (buffers gaps).
///     Used by `fetchAudioToCache` → AudioStreamProxy so media_kit can
///     start decoding before the download finishes. The two views share
///     storage so a single receive feeds both.
class _AudioReceiver {
  _AudioReceiver(this.totalBytes);

  final int totalBytes;
  final Completer<List<int>> _done = Completer();
  final StreamController<Uint8List> _stream =
      StreamController<Uint8List>(sync: false);
  final Map<int, Uint8List>  _chunks = {};
  bool _gotEof = false;
  int  _maxSeq = -1;
  int  _nextEmit = 0;

  /// Bytes successfully received so far. Updated on every chunk arrival
  /// so UI surfaces can show real progress instead of an indeterminate
  /// spinner. Distinct from `totalBytes` (the header's promised size).
  int _received = 0;
  int get received => _received;

  /// Wall-clock of the last chunk arrival. Callers polling this can
  /// detect stalled streams without subscribing to the byte stream.
  DateTime _lastChunkAt = DateTime.now();
  DateTime get lastChunkAt => _lastChunkAt;

  /// Subscribe-style progress fanout. Each `feed()` notifies listeners
  /// AFTER updating `_received`. Listeners are responsible for catching
  /// their own throws; we never let one bad listener stall the receiver.
  final List<void Function(int received, int total)> _progress = [];
  void onProgress(void Function(int received, int total) cb) {
    _progress.add(cb);
  }

  Future<List<int>>     get future => _done.future;
  Stream<Uint8List>     get stream => _stream.stream;

  void feed(int seq, Uint8List payload, bool eof) {
    // Only count bytes the first time a seq lands — duplicate chunks
    // (rare, but possible if a retransmit races the first arrival)
    // shouldn't inflate progress.
    if (!_chunks.containsKey(seq)) {
      _received += payload.length;
    }
    _chunks[seq] = payload;
    _lastChunkAt = DateTime.now();
    if (seq > _maxSeq) _maxSeq = seq;
    if (eof) _gotEof = true;

    for (final cb in _progress) {
      try { cb(_received, totalBytes); } catch (_) {}
    }

    // Emit every contiguous chunk we now have starting at _nextEmit so
    // the HTTP proxy can hand bytes to media_kit without waiting for EOF.
    while (_chunks.containsKey(_nextEmit)) {
      _stream.add(_chunks[_nextEmit]!);
      _nextEmit++;
    }

    // Only complete the all-bytes future once eof has fired and every
    // seq 0.._maxSeq is present (a defensive check — in practice TCP
    // delivers in order so _nextEmit catches up to _maxSeq+1).
    if (!_gotEof) return;
    for (int i = 0; i <= _maxSeq; ++i) {
      if (!_chunks.containsKey(i)) return;
    }
    final out = BytesBuilder(copy: false);
    for (int i = 0; i <= _maxSeq; ++i) {
      out.add(_chunks[i]!);
    }
    _chunks.clear();
    if (!_done.isCompleted) _done.complete(out.takeBytes());
    if (!_stream.isClosed) _stream.close();
  }

  /// Caller bailed (timeout, peer dropped). Tear the stream down so the
  /// HTTP proxy notices and closes its response.
  void cancel() {
    if (!_done.isCompleted) {
      _done.completeError(StateError('audio stream cancelled'));
    }
    if (!_stream.isClosed) _stream.close();
  }
}

/// Public handle returned by [RatsClient.streamFromSwarm]. Carries the
/// total byte count (so the HTTP proxy can advertise Content-Length and
/// libmpv computes track duration up front), an in-order chunk stream,
/// and a `cancel` to tear the swarm fetch down if the consumer abandons.
class AudioStream {
  AudioStream._(this._streamId, this.totalBytes, this._receiver, this._owner);

  final int                          _streamId;
  final int                          totalBytes;
  final _AudioReceiver               _receiver;
  final Map<int, _AudioReceiver>     _owner;

  Stream<Uint8List> get bytes => _receiver.stream;
  Future<void>      get done  => _receiver.future.then((_) => null);

  void cancel() {
    _receiver.cancel();
    _owner.remove(_streamId);
  }
}

/// A single swarm member's encoding of a song. Two players that uploaded
/// Madonna - Holiday in 128 kbps mp3 vs 320 kbps flac become two
/// variants under the same canonical chain entry — the streaming path
/// defaults to the lowest bitrate, the download dialog lets the user
/// pick which quality to keep.
class SwarmVariant {
  const SwarmVariant({
    required this.peerId,
    required this.contentHash,
    required this.bitrate,
    required this.audioFormat,
  });

  final String peerId;       // 40-hex librats peer id
  final String contentHash;  // peer's local content_hash (== canonical when
                             // the peer was the first submitter)
  final int    bitrate;      // bits/sec, 0 = unknown
  final String audioFormat;  // 'mp3' | 'ogg'

  /// Quantised bitrate in kbps, snapped to the nearest 16 kbps bucket.
  /// Different decoders report 128 kbps mp3 files at 128001 / 127999 /
  /// 128512 etc — without snapping they show up as three separate
  /// options in the download picker. 0 means "unknown".
  int get qualityBucketKbps {
    if (bitrate <= 0) return 0;
    final kbps = (bitrate / 1000).round();
    return ((kbps + 8) ~/ 16) * 16;
  }

  /// Stable dedup key for the download dialog — variants with the same
  /// snapped bitrate AND format collapse into one row.
  String get qualityKey => '$qualityBucketKbps|${audioFormat.toLowerCase()}';

  /// Human-readable label for the download picker UI.
  String get qualityLabel {
    final fmt = audioFormat.toUpperCase();
    final bucket = qualityBucketKbps;
    if (bucket <= 0) return '$fmt (unknown bitrate)';
    return '$bucket kbps $fmt';
  }
}
