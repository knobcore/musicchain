// SwarmRegistry — peer-to-peer source discovery for content hashes.
//
// Responsibilities:
//   * Track every content_hash this player currently holds (driven by
//     LibraryService); whenever the set changes, re-announce.
//   * Periodically announce each held hash to the librats DHT so other
//     peers can find us even when the VPS is unreachable.
//   * Resolve a given hash to a list of peer sources by asking the DHT
//     and, in parallel, the full node's swarm.locate / stream.open verb
//     (so we still get peer ids the DHT might not yet know about during
//     bootstrap).
//
// Content hashes on bopwire are SHA-256 (64 hex chars), but the DHT
// works on 20-byte BitTorrent info-hashes (40 hex). We derive a stable
// DHT key by SHA-1-hashing the 32 raw SHA-256 bytes. Every peer that
// implements this derivation lands on the same DHT bucket for the same
// song, regardless of whether they speak from chain metadata or from a
// raw file hash.

import 'dart:async';
import 'dart:typed_data';

import 'package:crypto/crypto.dart' as crypto;

import 'library_service.dart';
import 'rats_client.dart';

/// A potential source for a content_hash, normalized to the form the
/// PieceDownloader can dial directly.
class PeerSource {
  /// Either a librats peer_id (40 hex, when we already have a handshake
  /// with this peer) OR empty when we only know the network address.
  /// The downloader resolves missing peer_ids via librats's handshake.
  final String peerId;

  /// "host:port" the peer is reachable on, when known. Either populated
  /// from a DHT response (raw socket address) or from the full node's
  /// swarm reply (when it includes public_address). Empty when the only
  /// path is to relay through the VPS.
  final String address;

  /// Source of the discovery — useful for logs and prioritizing direct
  /// over relayed peers.
  final String origin;

  PeerSource({required this.peerId, required this.address, required this.origin});

  @override
  String toString() =>
      'PeerSource($origin, peer=${peerId.isEmpty ? "?" : peerId.substring(0, 12)}, addr=$address)';
}

class SwarmRegistry {
  SwarmRegistry._(this._rats);

  static SwarmRegistry? _instance;
  static Future<SwarmRegistry> initialize() async {
    if (_instance != null) return _instance!;
    final inst = SwarmRegistry._(RatsClient.instance);
    _instance = inst;
    // ignore: discarded_futures
    unawaited(inst._bootstrap());
    return inst;
  }

  static SwarmRegistry get instance {
    final i = _instance;
    if (i == null) throw StateError('SwarmRegistry.initialize() not called');
    return i;
  }

  final RatsClient _rats;

  /// Held content hashes (SHA-256 hex). Maintained in sync with
  /// [LibraryService.entries] so adding a downloaded song re-announces
  /// it within the next refresh tick.
  final Set<String> _heldHashes = {};

  /// Cache of `<sha256> -> List<PeerSource>` from the last DHT lookup.
  /// Indexed lookups during a PieceDownloader run hit this cache so
  /// we're not spamming DHT round-trips per piece.
  ///
  /// `LinkedHashMap` preserves insertion order so the LRU eviction
  /// below can drop the oldest entry without a full scan. 256 unique
  /// hashes is comfortably above the working set of any download
  /// session — bigger than that and the resident memory starts to
  /// matter, smaller and we'd flush hot entries needlessly.
  static const _kSourceCacheCap = 256;
  final Map<String, _CachedSources> _sourceCache = <String, _CachedSources>{};

  void _capSourceCache() {
    while (_sourceCache.length > _kSourceCacheCap) {
      _sourceCache.remove(_sourceCache.keys.first);
    }
  }

  Timer? _announceTimer;
  bool _bootstrapped = false;

  /// DHT key derivation: SHA-1 of the raw SHA-256 bytes. Stable across
  /// every peer implementing this method, so look-ups land in the same
  /// DHT bucket.
  static String dhtKeyFor(String sha256Hex) {
    if (sha256Hex.length != 64) {
      throw ArgumentError('content_hash must be 64-hex SHA-256');
    }
    final raw = Uint8List(32);
    for (int i = 0; i < 32; ++i) {
      raw[i] = int.parse(sha256Hex.substring(i * 2, i * 2 + 2), radix: 16);
    }
    final d = crypto.sha1.convert(raw).bytes;
    final sb = StringBuffer();
    for (final b in d) {
      sb.write(b.toRadixString(16).padLeft(2, '0'));
    }
    return sb.toString();
  }

  Future<void> _bootstrap() async {
    final lib = LibraryService.instance;
    await lib.ensureLoaded();
    _heldHashes.addAll(_localHashes(lib));
    lib.addListener(_onLibraryChanged);

    // Wait for the DHT to actually finish starting before firing
    // announces — librats reports isDhtRunning after the UDP socket
    // binds + the routing-table seed completes. Without the wait the
    // first batch of announces races the bootstrap and silently
    // no-ops. 5-second cap so a node with a broken DHT (firewall
    // blocks UDP) still finishes initialize and proceeds.
    final deadline = DateTime.now().add(const Duration(seconds: 5));
    while (!_rats.isDhtRunning && DateTime.now().isBefore(deadline)) {
      await Future<void>.delayed(const Duration(milliseconds: 250));
    }

    // First announce sweep, then re-announce every 15 minutes.
    // librats's DHT decays peer records after ~30 min, so 15 keeps us
    // permanently in the bucket without thrashing.
    _bootstrapped = true;
    unawaited(_announceAll());
    _announceTimer = Timer.periodic(
        const Duration(minutes: 15), (_) => _announceAll());
  }

  Iterable<String> _localHashes(LibraryService lib) sync* {
    for (final e in lib.entries) {
      if (e.isLocal && e.contentHash.length == 64) yield e.contentHash;
    }
  }

  void _onLibraryChanged() {
    final lib = LibraryService.instance;
    final fresh = _localHashes(lib).toSet();
    final added = fresh.difference(_heldHashes);
    _heldHashes
      ..clear()
      ..addAll(fresh);
    if (added.isNotEmpty) {
      // Newly-downloaded song: announce immediately rather than waiting
      // for the next 15-min tick so other peers can pick it up right
      // away.
      for (final h in added) {
        unawaited(_announceOne(h, logTag: 'fresh'));
      }
    }
  }

  Future<void> _announceAll() async {
    if (!_bootstrapped) return;
    // Don't await — fire all announces in parallel with a tiny stagger
    // so we don't slam librats's DHT layer with 150+ simultaneous
    // UDP sends. The announces themselves are eventually-consistent so
    // ordering doesn't matter.
    final hashes = _heldHashes.toList();
    for (int i = 0; i < hashes.length; ++i) {
      // ignore: discarded_futures
      unawaited(_announceOne(hashes[i], logTag: 'tick'));
      if (i % 16 == 15) {
        await Future<void>.delayed(const Duration(milliseconds: 50));
      }
    }
  }

  Future<void> _announceOne(String sha256, {required String logTag}) async {
    if (!_rats.isDhtRunning) return;
    final dhtKey = dhtKeyFor(sha256);
    // Pure announce — no peer callback. librats keeps invoking the
    // peers-found callback over the DHT lookup window (~10s), so
    // attaching one for our own library would either leak trampolines
    // or trip the Dart VM's "callback invoked after deletion" guard.
    _rats.announceOnly(dhtKey);
    // Surface for log greps; loop tag distinguishes startup vs tick.
    if (logTag == 'fresh') {
      // ignore: avoid_print
      print('[dht] announce fresh ${sha256.substring(0, 12)}…');
    }
  }

  /// Resolve [sha256] to a list of peer sources. Hits the DHT for one
  /// fresh round-trip (or returns a recent cache), then merges with any
  /// existing connected peers we believe also hold this hash.
  ///
  /// Returns an empty list when neither the DHT nor the connection set
  /// can offer a candidate — the caller should fall back to the legacy
  /// `stream.open` path against the full node.
  Future<List<PeerSource>> findSources(String sha256) async {
    final cached = _sourceCache[sha256];
    if (cached != null && cached.fresh) return cached.sources;

    final out = <String, PeerSource>{};

    if (_rats.isDhtRunning) {
      try {
        final dhtKey = dhtKeyFor(sha256);
        final addrs = await _rats.findHashHolders(dhtKey);
        for (final a in addrs) {
          if (a.isEmpty) continue;
          out[a] = PeerSource(
              peerId: '', address: a, origin: 'dht');
        }
      } catch (_) {
        // DHT failure is non-fatal — fall through.
      }
    }

    final fresh = _CachedSources(out.values.toList(growable: false));
    _sourceCache.remove(sha256); // re-insert to move to end (LRU)
    _sourceCache[sha256] = fresh;
    _capSourceCache();
    return fresh.sources;
  }

  void mergeSources(String sha256, Iterable<PeerSource> extras) {
    final existing = _sourceCache[sha256]?.sources ?? const <PeerSource>[];
    final byKey = <String, PeerSource>{};
    for (final s in existing) {
      byKey[s.peerId.isNotEmpty ? s.peerId : s.address] = s;
    }
    for (final s in extras) {
      final key = s.peerId.isNotEmpty ? s.peerId : s.address;
      if (key.isEmpty) continue;
      byKey.putIfAbsent(key, () => s);
    }
    _sourceCache.remove(sha256);
    _sourceCache[sha256] = _CachedSources(
        byKey.values.toList(growable: false));
    _capSourceCache();
  }

  /// Tear down the periodic announce and unhook the LibraryService
  /// listener. Idempotent — calling more than once is safe. Tests reset
  /// the singleton via [resetForTest] below; production code never
  /// disposes, but the listener removal is still correct hygiene.
  bool _disposed = false;
  void dispose() {
    if (_disposed) return;
    _disposed = true;
    _announceTimer?.cancel();
    _announceTimer = null;
    try {
      LibraryService.instance.removeListener(_onLibraryChanged);
    } catch (_) {
      // LibraryService may already be torn down in test contexts.
    }
    _sourceCache.clear();
    _heldHashes.clear();
  }

  /// Test-only: drop the singleton + dispose the existing instance so a
  /// fresh `initialize()` re-attaches listeners cleanly.
  static void resetForTest() {
    final i = _instance;
    _instance = null;
    i?.dispose();
  }
}

class _CachedSources {
  _CachedSources(this.sources) : at = DateTime.now();
  final List<PeerSource> sources;
  final DateTime         at;
  bool get fresh =>
      DateTime.now().difference(at) < const Duration(seconds: 90);
}
