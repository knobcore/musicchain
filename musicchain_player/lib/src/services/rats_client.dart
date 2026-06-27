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

import 'package:crypto/crypto.dart';
import 'package:ffi/ffi.dart';
import 'package:flutter/services.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../ffi/rats_bindings.dart';
import 'player_server.dart';
import 'wallet_service.dart';   // #10: sign relay.receipt with the wallet key

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
/// Hard ceiling on an inbound librats binary frame. Legit frames are an audio
/// chunk (~16 KB) or a download piece (≤256 KB); anything larger is a
/// malformed/hostile peer and would OOM-kill the app when copied. Dropped in
/// the binary callback before any allocation.
const int _kMaxBinaryFrame = 8 * 1024 * 1024;

class _PendingRequest {
  final Completer<Object?> completer; // body may be Map, List, primitive, null
  final Timer timeout;
  // (#4 instability fix) the librats peer this request was actually sent to
  // — for a relayed RPC that's the relay mini-node, not the ultimate target.
  // _handlePeerDisconnected uses it to fail-fast every in-flight request
  // whose relay/peer just died instead of waiting out the 15s/8s timeout.
  final String destPeer;
  _PendingRequest(this.completer, this.timeout, this.destPeer);
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

  // (#17 instability fix) Receiver-allocated stream ids. We propose a unique
  // id per fetch (client_stream_id) to the serving peer so two serving peers
  // can't independently pick the same random 32-bit id and collide in our
  // _streams map (which would misroute audio chunks). Monotonic, wrapped to
  // 32 bits, and skipping any id currently active in _streams.
  int _clientStreamSeq = 0;
  int _allocClientStreamId() {
    for (int i = 0; i < 1 << 16; ++i) {
      _clientStreamSeq = (_clientStreamSeq + 1) & 0xFFFFFFFF;
      if (_clientStreamSeq != 0 && !_streams.containsKey(_clientStreamSeq)) {
        return _clientStreamSeq;
      }
    }
    return _clientStreamSeq;   // pathological fallback (≥65k live streams)
  }

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

  /// Most-recent load_score per mini-node peer id, populated from
  /// `mininodes.list` replies. Used by [bestMiniNodePeerId] to pick the
  /// lightest mini-node for relay traffic instead of always landing on
  /// `firstMiniNodePeerId`, which under load could concentrate every
  /// player on a single VPS. Empty until the first mininodes.list reply
  /// has been merged in — callers fall back to [firstMiniNodePeerId] in
  /// that window.
  final Map<String, double> _miniNodeLoad = {};
  final Map<String, int>    _miniNodeLastSeenMs = {};

  /// External hook for any caller that has fresh mininodes.list data and
  /// wants to keep the load-aware picker current (librats_discovery's
  /// 20s bidirectional probe is one such caller). Pass `peer_id ->
  /// load_score` pairs; entries not present in the map are left
  /// untouched so partial updates don't blow away known scores.
  void updateMiniNodeLoad(Map<String, double> loadByPeerId,
                          {Map<String, int>? lastSeenMs}) {
    if (loadByPeerId.isEmpty && (lastSeenMs == null || lastSeenMs.isEmpty)) {
      return;
    }
    _miniNodeLoad.addAll(loadByPeerId);
    if (lastSeenMs != null) _miniNodeLastSeenMs.addAll(lastSeenMs);
  }

  /// Read-only view onto the per-mini-node load_score cache so callers
  /// like LibratsDiscovery can introspect for diagnostics.
  Map<String, double> get miniNodeLoadScores =>
      Map.unmodifiable(_miniNodeLoad);

  /// Load-aware mini-node selector. Walks `_miniNodePeerIds` intersected
  /// with `validatedPeerIds`, then picks the entry with the lowest
  /// cached `load_score`. Tiebreaks on freshest `last_seen_ms`. Returns
  /// null when no validated mini-node is connected; falls back to
  /// [firstMiniNodePeerId] when the load cache hasn't populated yet
  /// (cold start, first ~20 s after launch).
  String? get bestMiniNodePeerId {
    if (!_started) return null;
    final validated = validatedPeerIds.toSet();
    final eligible = _miniNodePeerIds
        .where((id) => validated.contains(id))
        .toList(growable: false);
    if (eligible.isEmpty) return null;
    if (_miniNodeLoad.isEmpty) return firstMiniNodePeerId;
    eligible.sort((a, b) {
      final la = _miniNodeLoad[a] ?? double.infinity;
      final lb = _miniNodeLoad[b] ?? double.infinity;
      final cmp = la.compareTo(lb);
      if (cmp != 0) return cmp;
      // Tiebreak: prefer the freshest mini-node (highest last_seen_ms).
      final sa = _miniNodeLastSeenMs[a] ?? 0;
      final sb = _miniNodeLastSeenMs[b] ?? 0;
      return sb.compareTo(sa);
    });
    return eligible.first;
  }

  /// Per-target relay routing. Populated by `LibratsDiscovery` from the
  /// `reachability` field returned by routes.get. When set, requests bound
  /// for `targetPeerId` are wrapped as `relay.forward` and sent to the
  /// associated relay peer (the mini-node) instead of being sent direct.
  final Map<String, String> _relayVia = {};

  /// #10: the loaded wallet, injected from main(), used to sign
  /// relay.receipt. WalletService is not a singleton, so a bare
  /// WalletService() would have no loaded key — main() must set this to the
  /// same instance that holds the player's wallet.
  WalletService? _wallet;
  set wallet(WalletService w) => _wallet = w;

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
      // Treat the native count as untrusted: a garbage/huge n would walk
      // arr.elementAt(i) past the allocation (SIGSEGV) and free junk pointers.
      // A real client has at most a few hundred peers.
      if (arr.address == 0 || n <= 0 || n > 100000) return out;
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

  // (DHT un-nerf P0b) serializes connectAndResolve so concurrent dials can't
  // misattribute which newly-validated peer_id belongs to which dial.
  Future<void> _dialResolveChain = Future<void>.value();

  /// Dial a raw host:port that carries no librats peer_id (e.g. a
  /// DHT-discovered seeder) and return the peer_id that validates as a result.
  /// librats exposes no address→peer_id lookup, so we snapshot the validated
  /// set, dial, and return the newly-appeared id. Serialized via a chained
  /// future (Dart is single-isolate, so the synchronous prefix that swaps the
  /// chain is atomic) so two concurrent dials don't both claim the same fresh
  /// id. Returns null if nothing new validates within [timeout].
  Future<String?> connectAndResolve(String host, int port,
      {Duration timeout = const Duration(seconds: 6)}) async {
    if (!_started || host.isEmpty || port <= 0) return null;
    final prev = _dialResolveChain;
    final gate = Completer<void>();
    _dialResolveChain = gate.future;
    await prev;
    try {
      final before = validatedPeerIds.toSet();
      connect(host, port);
      final deadline = DateTime.now().add(timeout);
      while (DateTime.now().isBefore(deadline)) {
        await Future.delayed(const Duration(milliseconds: 200));
        final fresh = validatedPeerIds.toSet().difference(before);
        if (fresh.isNotEmpty) return fresh.first;
      }
      return null;
    } finally {
      gate.complete();
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

    // Register the disconnect callback so we can tear down per-peer state
    // (mini-node membership, relay mappings, and any in-flight outbound
    // streams PlayerServer is serving to that peer) the moment librats
    // notices the peer is gone, rather than waiting for the next watchdog
    // tick to catch validatedPeerIds shrinking.
    _registerDisconnectHandler();

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
    // (discovery fix) An explicit network change means the full node has very
    // likely already dropped our swarm entries (our socket died on its side),
    // so our library MUST be re-announced once we reconnect or our songs go
    // undiscoverable even though we're online. NEVER let the flap cooldown
    // suppress that: reset the hysteresis + cooldown so the next stable-up
    // tick fires onVpsReconnected → LibraryScanner.reAnnounce(). That path is
    // digest-checked (syncSwarm only resubmits what the home node is actually
    // missing), so firing it on every network change is cheap and idempotent.
    _upSince = null;
    _lastReconnectFired = null;
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
    final freshLoad = <String, double>{};
    final freshSeen = <String, int>{};
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
          // Cache load_score keyed by the mini-node's rats peer id so
          // bestMiniNodePeerId can pick the lightest relay. mininodes
          // that omit either field are skipped — bestMiniNode... treats
          // missing scores as infinity so they sort last.
          final entryPid =
              (e['rats_peer_id'] as String? ?? '').trim();
          if (entryPid.isNotEmpty) {
            final ls = (e['load_score'] as num?)?.toDouble();
            if (ls != null) freshLoad[entryPid] = ls;
            final seen = (e['last_seen_ms'] as int?);
            if (seen != null) freshSeen[entryPid] = seen;
          }
        }
      } catch (_) { /* peer didn't implement mininodes.list — skip */ }
    }
    if (freshLoad.isNotEmpty || freshSeen.isNotEmpty) {
      updateMiniNodeLoad(freshLoad, lastSeenMs: freshSeen);
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
  // (#6 instability fix) hysteresis + rate-limit for the heavy reconnect
  // recovery (full library re-announce + onVpsReconnected). A flapping
  // wifi↔cellular link would otherwise re-fire that storm on every up-edge.
  DateTime? _upSince;            // when the link most recently came up
  DateTime? _lastReconnectFired; // last time we ran the heavy recovery
  static const Duration _kStableUp          = Duration(seconds: 3);
  static const Duration _kReconnectCooldown = Duration(seconds: 20);

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
      // Drop ghost mini-node peer ids and stale relay mappings so the
      // next refresh repopulates from scratch instead of routing through
      // peer ids librats no longer knows. After a mini-node restart
      // there's a ~30 s window where the old peer_id is still cached
      // here but the underlying socket is dead — without this every
      // outbound request gets routed to the ghost peer and times out at
      // 15 s instead of failing fast.
      _miniNodePeerIds.clear();
      _relayVia.clear();
      _vpsWasUp = false;
      _upSince  = null;
      return;
    }
    // Link is up. Only run the heavy recovery once per up-period, AND only
    // after it has been stably up for _kStableUp (so a momentary flap
    // doesn't trigger it), AND at most once per _kReconnectCooldown.
    if (_vpsWasUp) return;
    final now = DateTime.now();
    _upSince ??= now;
    if (now.difference(_upSince!) < _kStableUp) return;   // not stable yet
    if (_lastReconnectFired != null &&
        now.difference(_lastReconnectFired!) < _kReconnectCooldown) {
      _vpsWasUp = true;   // within cooldown — mark handled, skip the storm
      return;
    }
    _vpsWasUp = true;
    _lastReconnectFired = now;
    // Re-announce ourselves + retry the public-address probe so other
    // peers can locate us, and tell upper layers to re-join swarms.
    unawaited(_announceToVps());
    unawaited(_observePublicAddressViaVps());
    try { onVpsReconnected?.call(); } catch (_) {}
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
  /// Evicted DHT callbacks are RETIRED here, never closed while librats is
  /// live. librats's DHT can still deliver a late peers-found batch for an
  /// evicted key; closing the trampoline first aborts the VM with
  /// "Callback invoked after it has been deleted" (the exact crash we hit).
  /// dispose() closes these only AFTER _b.stop() halts the worker threads —
  /// the same invariant the dispose teardown already documents.
  final List<NativeCallable<NativePeersFoundCb>>       _retiredDhtCallbacks = [];

  void _capDhtRegistry() {
    while (_dhtCallbacks.length > _kDhtRegistryCap) {
      final oldest = _dhtCallbacks.keys.first;
      // Retire, don't close: a late DHT batch for this key would invoke a
      // freed trampoline → SIGABRT. These are closed safely in dispose().
      final cb = _dhtCallbacks.remove(oldest);
      if (cb != null) _retiredDhtCallbacks.add(cb);
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

  /// Locate peers seeding [contentHashHex] via the BitTorrent-compatible
  /// DHT. Full nodes announce `content_hash` against the DHT key
  /// `sha1(content_hash_bytes)` (BEP-5 style) as part of
  /// `fingerprint.submit`; players call this to discover seeders
  /// independently of the swarm.hello / stream.open path so a song
  /// remains reachable even when no mini-node has cached the swarm map.
  Future<List<String>> findContentSeeders(
      String contentHashHex,
      {Duration timeout = const Duration(seconds: 4)}) async {
    // Must be a 32-byte content hash rendered as 64 hex chars.
    if (contentHashHex.length != 64) return const <String>[];
    final hashBytes = <int>[];
    for (int i = 0; i < contentHashHex.length; i += 2) {
      final hi = int.tryParse(contentHashHex.substring(i, i + 2), radix: 16);
      if (hi == null) return const <String>[];
      hashBytes.add(hi);
    }
    final dhtKey = sha1.convert(hashBytes).bytes;
    final buf = StringBuffer();
    for (final b in dhtKey) {
      buf.write(b.toRadixString(16).padLeft(2, '0'));
    }
    final dhtKeyHex = buf.toString();
    return findHashHolders(dhtKeyHex, listenWindow: timeout);
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
    // (#crash) Detach the static callback slot + flip _started BEFORE we
    // stop/destroy the native client. `_b.stop`/`_b.destroy` can fire
    // disconnect callbacks synchronously while tearing peers down; with the
    // slot already null those trampolines bail immediately instead of
    // re-entering Dart state (or, worse, the native handle) during teardown.
    if (identical(_activeForCallback, this)) {
      _activeForCallback = null;
    }
    _started = false;
    // Close every persistent DHT NativeCallable. Closing forfeits the
    // safety invariant librats relies on (the trampoline outlives any
    // late-arriving peer batch), so we tear the rats client down FIRST
    // — `_b.stop` halts librats's worker threads, which guarantees the
    // DHT layer won't fire another callback after this point.
    _b.stop(_handle);
    _b.destroy(_handle);
    // Close every NativeCallable ONLY after destroy(). stop() may merely
    // SIGNAL the librats worker threads to wind down, but destroy() joins
    // them — so this is the one point where no DHT / message / connection
    // callback can still fire into a trampoline we're about to free. Closing
    // them before destroy (as the old code did, trusting stop() alone) is the
    // race behind "Callback invoked after it has been deleted".
    for (final cb in _dhtCallbacks.values) {
      try { cb.close(); } catch (_) {}
    }
    for (final cb in _retiredDhtCallbacks) {
      try { cb.close(); } catch (_) {}
    }
    _retiredDhtCallbacks.clear();
    _dhtCallbacks.clear();
    _dhtResults.clear();
    _replyCallable.close();
    _requestCallable.close();
    _binaryCallable.close();
    _disconnectCallable.close();
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
        //
        // bestMiniNodePeerId is load-aware (lightest mini-node first,
        // tiebreak on freshness) so during a peak we don't concentrate
        // every player's relay traffic on a single VPS. Falls through
        // to firstMiniNodePeerId on cold start (before mininodes.list
        // populated the load cache) and finally to validatedPeerIds.first
        // as the universal last resort.
        routeVia = bestMiniNodePeerId
            ?? firstMiniNodePeerId
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
    _pending[reqId] = _PendingRequest(completer, t, peerId);

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
    } catch (_) {
      // We're inside a native librats callback — a Dart exception escaping
      // here crosses the C boundary and ABORTS the process. Swallow anything
      // _dispatchReply throws (e.g. a bad `as Map` cast on a hostile reply).
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
      // Bound `size` BOTH ways before touching the buffer: < 9 has no header,
      // and an absurdly large frame (untrusted peer) would double via fromList
      // and OOM-kill the app on Android. Legit chunks are ~16 KB (audio) /
      // ≤256 KB (piece); 8 MB is a generous ceiling. Drop oversized frames.
      if (size < 9 || size > _kMaxBinaryFrame || dataPtr.address == 0) return;
      final bytes = dataPtr.cast<Uint8>().asTypedList(size);
      final copy  = Uint8List.fromList(bytes);
      self._dispatchBinary(copy);
    } catch (_) {
      // Native callback boundary — never let a Dart throw (e.g. feed() racing
      // a closed StreamController) cross into C and abort the process.
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

  late final NativeCallable<NativeConnCb> _disconnectCallable;

  /// Static trampoline for peer-disconnect events. librats hands us a
  /// strdup'd peer_id we must NOT free (per project_librats_callback
  /// ownership: connection_cb strdups, disconnect_cb does NOT — freeing
  /// it yields STATUS_HEAP_CORRUPTION). We copy the id into Dart and
  /// hand off to the per-peer cleanup.
  static void _onDisconnectListener(
      Pointer<Void> _, Pointer<Utf8> peerIdPtr) {
    final self = _activeForCallback;
    if (self == null) return;
    if (peerIdPtr.address == 0) return;
    String peerId;
    try {
      peerId = peerIdPtr.toDartString();
    } catch (_) {
      return;
    }
    // This runs inside a native librats callback — an exception escaping it
    // crosses the C boundary and ABORTS the process (SIGABRT). Never let one
    // out.
    try {
      self._handlePeerDisconnected(peerId);
    } catch (_) {/* swallow — a disconnect cleanup throw must not crash */}
  }

  void _registerDisconnectHandler() {
    _disconnectCallable =
        NativeCallable<NativeConnCb>.listener(_onDisconnectListener);
    _b.setDisconnectCb(_handle, _disconnectCallable.nativeFunction, nullptr);
  }

  void _handlePeerDisconnected(String peerId) {
    if (peerId.isEmpty) return;
    // Drop per-peer routing state so subsequent requests don't get
    // relayed through a dead mini-node id.
    _miniNodePeerIds.remove(peerId);
    _miniNodeLoad.remove(peerId);
    _miniNodeLastSeenMs.remove(peerId);
    _relayVia.removeWhere((_, via) => via == peerId);

    // Tell PlayerServer to cancel any in-flight outbound streams targeted
    // at this peer so the chunk loop bails between sends instead of
    // blasting kilobytes into a closed socket. Guarded: PlayerServer.instance
    // throws StateError before PlayerServer.initialize() has run (test
    // harness, early dispose race), and any throw inside the cancel path
    // must not break the disconnect event delivery.
    try {
      PlayerServer.instance.cancelStreamsForPeer(peerId);
    } catch (_) {/* PlayerServer not initialized yet, or cancel threw */}

    // (#4 instability fix) Fail-fast every in-flight request whose
    // destination (a relay mini-node or a direct peer) just died. Without
    // this, a mini-node drop / cellular-flip leaves each orphaned request
    // blocked on its 15s/8s timer (an album batch stacks these into
    // multi-minute UI stalls). We surface 'send_failed' — the status the
    // catalog retry (LibraryProvider._withRediscoverRetry) and the audio
    // swarm-member loop already treat as retryable — so recovery starts
    // immediately on a surviving mini-node instead of after the timeout.
    final dead = <String>[];
    _pending.forEach((reqId, p) {
      if (p.destPeer == peerId) dead.add(reqId);
    });
    for (final reqId in dead) {
      final p = _pending.remove(reqId);
      p?.timeout.cancel();
      if (p != null && !p.completer.isCompleted) {
        p.completer.completeError(
            RatsRpcException('send_failed', 'relay/peer $peerId disconnected'));
      }
    }
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
    // 1. Ask the full node who has the file. (#crash) Cast defensively — a
    // null/non-map reply would TypeError here, and fetchAudioToCache's
    // `on RatsRpcException` would NOT catch it, so it'd escape as an
    // unhandled async error. Empty map → no peers → handled as no_swarm.
    final probeRaw = await request(nodePeerId, 'stream.open',
        {'content_hash': contentHash},
        timeout: const Duration(seconds: 8));
    final probe = probeRaw is Map
        ? probeRaw.cast<String, dynamic>() : <String, dynamic>{};
    // #10: the full node (broker) mints a delivery_id and returns it on
    // both the no_swarm and swarm reply bodies. Thread it into the
    // per-peer stream.open so any relaying mini-node stamps it into the
    // F-frame, then the receipt goes back to nodePeerId (the broker).
    final deliveryId = probe['delivery_id'] as String?;
    // (#12) defensive parse; only take the legacy local fast-path when both
    // fields are well-formed, else fall through to the swarm path.
    final localSid = (probe['stream_id']   as num?)?.toInt();
    final localTot = (probe['total_bytes'] as num?)?.toInt();
    if ((probe['source'] == 'local' || probe['stream_id'] != null) &&
        localSid != null && localTot != null) {
      // Full node has the bytes (legacy path; new arch should never hit
      // this but keep it as a fallback). Bytes are already coming.
      final streamId   = localSid;
      final totalBytes = localTot;
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
            deliveryId:   deliveryId,
            brokerPeerId: nodePeerId,
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
          bitrate:     (m['bitrate'] as num?)?.toInt() ?? 0,  // (#crash) JSON may send double
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
    final probeRaw = await request(nodePeerId, 'stream.open',
        {'content_hash': contentHash},
        timeout: const Duration(seconds: 8));
    final probe = probeRaw is Map           // (#crash) defensive; see download path
        ? probeRaw.cast<String, dynamic>() : <String, dynamic>{};
    final deliveryId = probe['delivery_id'] as String?;  // #10 broker id
    // (#12) defensive parse; only take the local fast-path when well-formed.
    final localSid = (probe['stream_id']   as num?)?.toInt();
    final localTot = (probe['total_bytes'] as num?)?.toInt();
    if ((probe['source'] == 'local' || probe['stream_id'] != null) &&
        localSid != null && localTot != null) {
      final streamId   = localSid;
      final totalBytes = localTot;
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
              {'content_hash': fetchHash(v),
               'client_stream_id': _allocClientStreamId(),   // #17
               if (deliveryId != null) 'delivery_id': deliveryId},
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
      // (#12 instability fix) defensive parse — a malformed/hostile serving
      // peer could send a non-int/absent stream_id|total_bytes; a raw
      // `as int` here is OUTSIDE the per-attempt try and would abort the
      // whole playback instead of just skipping this member.
      final streamId   = (reply['stream_id']   as num?)?.toInt();
      final totalBytes = (reply['total_bytes'] as num?)?.toInt();
      if (streamId == null || totalBytes == null) {
        lastError = RatsRpcException('not_matched',
            'peer ${v.peerId} sent malformed stream.open reply');
        continue;
      }
      final receiver = _AudioReceiver(totalBytes);
      _streams[streamId] = receiver;
      _drainEarlyChunks(streamId, receiver);
      // #10: when the stream finishes (EOF), send a signed relay.receipt to
      // the broker. Best-effort: a never-completing stream just never
      // corroborates (the mini's report alone earns nothing).
      if (deliveryId != null) {
        final servingPeer = v.peerId;
        final fh = fetchHash(v);
        receiver.future.then((_) {
          _sendRelayReceipt(
            brokerPeerId:  nodePeerId,
            deliveryId:    deliveryId,
            contentHash:   fh,
            miniPeerId:    _relayVia[servingPeer] ?? '',
            bytesReceived: receiver.received,
          );
        }).catchError((_) {});
      }
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
       String? deliveryId,    // #10: broker-minted id, threaded into stream.open
       String? brokerPeerId,  // #10: full node that minted it (gets the receipt)
       void Function(int received, int total)? onProgress}) async {
    final reply = await request(peerId, 'stream.open',
        {'content_hash': contentHash,
         'client_stream_id': _allocClientStreamId(),   // #17
         if (deliveryId != null) 'delivery_id': deliveryId},
        timeout: openTimeout);
    // (#crash) defensive — a non-map reply would TypeError; treat as not_matched.
    if (reply is! Map) {
      throw RatsRpcException('not_matched',
          'peer $peerId sent a non-map stream.open reply');
    }
    final meta = reply.cast<String, dynamic>();
    if (meta['matched'] == false) {
      // Peer no longer has the bytes (entry was removed locally between
      // the full node's swarm reply and our follow-up). Bail so the
      // caller tries the next swarm member.
      throw RatsRpcException('not_matched',
          'peer $peerId no longer has $contentHash');
    }
    // (#12 instability fix) defensive parse against a malformed serving peer.
    final streamId   = (meta['stream_id']   as num?)?.toInt();
    final totalBytes = (meta['total_bytes'] as num?)?.toInt();
    if (streamId == null || totalBytes == null) {
      throw RatsRpcException('not_matched',
          'peer $peerId sent malformed stream.open reply');
    }
    final receiver = _AudioReceiver(totalBytes);
    _streams[streamId] = receiver;
    if (onProgress != null) receiver.onProgress(onProgress);
    // Pick up any chunks that landed in the race window between the
    // server returning the reply and us reaching this line.
    _drainEarlyChunks(streamId, receiver);
    try {
      final bytes =
          await _awaitWithStallGuard(receiver, totalTimeout, chunkStall);
      // #10: send a signed relay.receipt to the BROKER (the full node that
      // minted the delivery_id and holds the pending-delivery row). Safe to
      // send unconditionally — the broker only credits when a mini-node's
      // signed byte-report ALSO arrives for the same delivery_id, so a
      // direct (non-relayed) fetch simply never corroborates.
      if (deliveryId != null && brokerPeerId != null) {
        unawaited(_sendRelayReceipt(
          brokerPeerId:  brokerPeerId,
          deliveryId:    deliveryId,
          contentHash:   contentHash,
          miniPeerId:    _relayVia[peerId] ?? '',
          bytesReceived: receiver.received,
        ));
      }
      return bytes;
    } finally {
      _streams.remove(streamId);
    }
  }

  /// #10: send a signed relay.receipt to the broker full node. Preimage:
  /// "relay.receipt" || delivery_id(16) || content_hash(32) || bytes(u64 LE).
  /// wallet.sign hashes internally (sha256), so we pass the raw bytes; the
  /// server verifies with verify_data. Best-effort — reward credit is not
  /// critical to playback.
  Future<void> _sendRelayReceipt({
    required String brokerPeerId,
    required String deliveryId,    // 32-hex (16 bytes)
    required String contentHash,   // 64-hex (32 bytes)
    required String miniPeerId,
    required int    bytesReceived,
  }) async {
    final wallet = _wallet;
    final info   = wallet?.info;
    if (wallet == null || info == null) return;
    final did = _hexToBytes(deliveryId);
    final ch  = _hexToBytes(contentHash);
    if (did.length != 16 || ch.length != 32) return;
    final msg = BytesBuilder();
    msg.add(utf8.encode('relay.receipt'));
    msg.add(did);
    msg.add(ch);
    final u64 = ByteData(8)..setUint64(0, bytesReceived, Endian.little);
    msg.add(u64.buffer.asUint8List());
    final String sig;
    try {
      sig = wallet.sign(Uint8List.fromList(msg.toBytes()));
    } catch (_) {
      return;   // wallet not loaded / sign failed — skip
    }
    try {
      await request(brokerPeerId, 'relay.receipt', {
        'delivery_id':    deliveryId,
        'content_hash':   contentHash,
        'mini_peer_id':   miniPeerId,
        'bytes_received': bytesReceived,
        'player_address': info.address,
        'player_pubkey':  info.publicKey,
        'sig':            sig,
      }, timeout: const Duration(seconds: 6));
    } catch (_) { /* best-effort */ }
  }

  Uint8List _hexToBytes(String hex) {
    final n = hex.length ~/ 2;
    final out = Uint8List(n);
    for (int i = 0; i < n; i++) {
      out[i] = (_rcHexNibble(hex.codeUnitAt(i * 2)) << 4) |
                _rcHexNibble(hex.codeUnitAt(i * 2 + 1));
    }
    return out;
  }

  int _rcHexNibble(int c) {
    if (c >= 0x30 && c <= 0x39) return c - 0x30;
    if (c >= 0x61 && c <= 0x66) return c - 0x61 + 10;
    if (c >= 0x41 && c <= 0x46) return c - 0x41 + 10;
    return 0;
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
  bool                  get isDone => _done.isCompleted;  // (#5) stall watchdog

  static const int _kMaxPendingChunks = 200000;  // flood guard (~3 GB @ 16 KB)

  void feed(int seq, Uint8List payload, bool eof) {
    // (#crash) Never touch a torn-down receiver. Once the future completed or
    // the byte stream closed (cancel / EOF), a late chunk arriving via the FFI
    // binary callback must not _stream.add — that throws "Cannot add event
    // after closing", and inside a native callback an escaping throw aborts
    // the process.
    if (_done.isCompleted || _stream.isClosed) return;
    // (#crash) seq is peer-controlled. An absurd value would blow up _maxSeq
    // and the assembly loop; a 1-byte-payload flood would grow _chunks
    // without bound (OOM). Reject out-of-range seq and cap pending chunks.
    if (seq < 0 || seq > totalBytes + 16) return;
    // Only count bytes the first time a seq lands — duplicate chunks
    // (rare, but possible if a retransmit races the first arrival)
    // shouldn't inflate progress.
    if (!_chunks.containsKey(seq)) {
      if (_chunks.length >= _kMaxPendingChunks) return;  // flood guard
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
    // seq 0.._maxSeq is present. (#crash) `_nextEmit` is the first not-yet-
    // contiguous seq (advanced by the emit loop above), so "all present" is
    // exactly `_nextEmit > _maxSeq` — O(1), instead of the old O(_maxSeq)
    // scan that a malicious large seq could turn into a billions-iteration
    // ANR.
    if (!_gotEof) return;
    if (_nextEmit <= _maxSeq) return;
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
  AudioStream._(this._streamId, this.totalBytes, this._receiver, this._owner,
      {Duration chunkStall = const Duration(seconds: 30)}) {
    // (#5/#15 instability fix) streamFromSwarm returns this AudioStream
    // immediately and nothing else aborts a receiver that simply stops
    // getting chunks (relay/peer death mid-track, no EOF). Without a guard
    // the receiver + its buffered bytes leak in _streams forever and the
    // loopback HTTP handler reading `bytes` hangs. Watchdog: if idle past
    // chunkStall before completion, cancel — errors the stream so the proxy
    // unblocks and the caller can retry, and frees the _streams entry.
    final stall = chunkStall;
    _stallTimer = Timer.periodic(stall ~/ 2, (_) {
      if (_receiver.isDone) { _stopWatch(); return; }
      if (DateTime.now().difference(_receiver.lastChunkAt) > stall) {
        cancel();
      }
    });
    // Also stop the watchdog the moment the stream finishes normally/errors.
    _receiver.future.whenComplete(_stopWatch).catchError((_) {});
  }

  final int                          _streamId;
  final int                          totalBytes;
  final _AudioReceiver               _receiver;
  final Map<int, _AudioReceiver>     _owner;
  Timer?                             _stallTimer;

  Stream<Uint8List> get bytes => _receiver.stream;
  Future<void>      get done  => _receiver.future.then((_) => null);

  void _stopWatch() { _stallTimer?.cancel(); _stallTimer = null; }

  void cancel() {
    _stopWatch();
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
