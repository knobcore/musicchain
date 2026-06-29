// Multi-peer, resumable, sha256-verified piece downloader.
//
// Replaces the legacy `requestAudio` single-peer streaming path. Given a
// content_hash, it:
//
//   * looks up peer sources via SwarmRegistry (DHT first, full-node fallback);
//   * keeps a persistent piece bitmap on disk so process restart / crash
//     resumes from the last verified byte;
//   * runs N parallel workers that each claim the next missing piece and a
//     peer (round-robin), request a 256 KB byte range via `audio.piece_get`,
//     and write it directly into the partial file. Workers SHARE a peer when
//     there are fewer peers than workers, so a single-peer download (e.g. one
//     relayed through the mini-node) still keeps many piece requests in flight
//     instead of fetching them serially round-trip-by-round-trip;
//   * swaps peers if a piece times out and re-queries DHT when the live
//     candidate list goes empty;
//   * verifies the assembled file's SHA-256 against the chain-block
//     content_hash before atomically renaming `.partial` → final cache.
//
// Wire layer is plain librats messaging; no librats patches needed. The
// peer-side `audio.piece_get` handler lives in PlayerServer.
//
// Persistent state per content_hash:
//   <cacheDir>/<hash>.partial   — sparse file holding raw bytes
//   <cacheDir>/<hash>.bitmap    — 1 byte per piece, 0 = missing, 1 = present
//   <cacheDir>/<hash>.meta      — JSON { total_size, piece_size, started_at_ms }
//
// All three live in the same dir so a single delete-the-directory wipe
// clears any half-finished downloads. None of the three are valid until
// the bitmap shows every piece complete; an interrupted download leaves
// .partial in place and the bitmap reflects which pieces are good.

import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:crypto/crypto.dart' as crypto;

import 'rats_client.dart';
import 'swarm_registry.dart';

/// Knobs intentionally exposed so tools/tests can override them without
/// reaching into private state.
class PieceDownloaderConfig {
  /// Bytes per piece — also the cap a peer can return in one call.
  /// 256 KB ≈ 350 KB on the wire after base64; large enough that round-
  /// trip overhead doesn't dominate, small enough that a slow peer
  /// blocking a single piece only stalls ~3 MB/s for the duration.
  final int pieceSize;

  /// Max concurrent piece-fetch workers. Workers round-robin over the peer
  /// pool and SHARE a peer when there are fewer peers than workers, so this is
  /// also the in-flight piece depth against a single (e.g. relayed) peer — the
  /// difference between latency-bound serial fetching (~15 KB/s over the VPS
  /// relay) and overlapping round-trips that saturate the available bandwidth.
  final int maxWorkers;

  /// Per-piece RPC timeout. A slow peer blowing this is treated as a
  /// stall, the piece goes back in the pool, and another worker picks
  /// it up.
  final Duration pieceTimeout;

  /// Cumulative piece errors from one peer before it's banned for this
  /// download. Kept generous because with many in-flight workers sharing a
  /// single relayed peer, a brief relay hiccup can rack up several stalls at
  /// once — banning the sole source on 2-3 of those would fail an otherwise
  /// recoverable download.
  final int perPeerRetryCap;

  const PieceDownloaderConfig({
    this.pieceSize        = 256 * 1024,
    // Raised 8 -> 16 so a LONE track can fill the adaptive congestion window
    // (_cwnd) and saturate a higher bandwidth-delay link instead of leaving the
    // pipe half-empty at 8 in-flight. The GLOBAL _cwnd still caps total
    // in-flight across all tracks, so this is per-track headroom, not 16x load.
    this.maxWorkers       = 16,
    // 30s (was 15): on a slow uplink a 256 KB piece can legitimately take >15s
    // to arrive; a 15s timeout pre-empts the in-flight reply and re-requests the
    // piece, double-serving it (the over-fetch loop). 30s tolerates a slow-but-
    // arriving reply so a piece is only re-requested when the source is truly stalled.
    this.pieceTimeout     = const Duration(seconds: 30),
    this.perPeerRetryCap  = 8,
  });
}

/// Result returned to the caller once a download settles.
class PieceDownloadResult {
  PieceDownloadResult(this.cachePath, this.totalBytes);
  final String cachePath;
  final int    totalBytes;
}

/// Thrown when integrity verification fails after every piece is in.
class IntegrityException implements Exception {
  IntegrityException(this.expectedSha256, this.actualSha256);
  final String expectedSha256;
  final String actualSha256;
  @override
  String toString() =>
      'IntegrityException(expected=$expectedSha256, actual=$actualSha256)';
}

/// Thrown when we exhaust every peer and the DHT and still can't
/// complete the download — callers can fall back to the legacy
/// stream.open path against the full node.
class NoPeerAvailableException implements Exception {
  NoPeerAvailableException(this.contentHash);
  final String contentHash;
  @override
  String toString() => 'NoPeerAvailableException(contentHash=$contentHash)';
}

class PieceDownloader {
  PieceDownloader({
    required this.contentHash,
    required this.cacheDir,
    required this.finalCachePath,
    this.config = const PieceDownloaderConfig(),
    this.onProgress,
    this.extraSources = const [],
  });

  /// 64-hex SHA-256 of the file (== chain's content_hash). Also the
  /// integrity oracle: PieceDownloader rejects assembled bytes that
  /// don't hash to this value.
  final String contentHash;

  /// Directory the partial / bitmap / meta files live in. Created on
  /// first use.
  final Directory cacheDir;

  /// Where the verified bytes end up. The downloader writes them to
  /// `<hash>.partial` first, hash-checks, then atomically renames to
  /// this path.
  final String finalCachePath;

  final PieceDownloaderConfig    config;
  final void Function(int got, int total)? onProgress;

  /// Optional peer-source list to seed the worker pool with — typically
  /// the swarm reply from the full node's `stream.open`, so we don't
  /// have to wait for a DHT round-trip before the first piece request
  /// goes out. SwarmRegistry's DHT lookup runs in parallel and tops up
  /// the pool as more peers reply.
  final List<PeerSource> extraSources;

  // ---- Internal state -------------------------------------------------

  String  get _partialPath => '${cacheDir.path}/$contentHash.partial';
  String  get _bitmapPath  => '${cacheDir.path}/$contentHash.bitmap';
  String  get _metaPath    => '${cacheDir.path}/$contentHash.meta';

  RandomAccessFile? _partialHandle;
  RandomAccessFile? _bitmapHandle;
  final _ioMutex = _Mutex();

  int               _totalSize = 0;
  int               _numPieces = 0;
  Uint8List         _bitmap    = Uint8List(0);
  int               _gotBytes  = 0;

  final Set<int>          _claimedPieces = {};
  final List<PeerSource>  _sources       = [];
  final Map<String, int>  _perPeerErrors = {};
  final Set<String>       _bannedPeers   = {};
  // (#8) Transient cooldown for stalled peers. A protocol violation is a hard
  // ban (the peer sent garbage); a stall is transient (relay hiccup, slow
  // pipe) and must NOT permanently ban the sole relayed source — that would
  // strand an otherwise-recoverable download. Instead the peer is parked here
  // until `cooldownUntil`, then becomes eligible again. _nextSource skips a
  // peer whose cooldown is still active but does NOT treat it as banned.
  final Map<String, DateTime> _cooldownUntil = {};
  static const Duration _kStallCooldown = Duration(seconds: 5);
  // (DHT un-nerf P0b) DHT-source dialing: address → resolved librats peer_id,
  // and addresses that never validated (don't redial them every piece).
  final Map<String, String> _dhtResolved = {};
  final Set<String>         _dhtBanned   = {};

  Object?              _terminalError;
  Completer<void>?     _terminalSignal;

  /// Run to completion. Returns the on-disk path of the verified file.
  /// Throws [IntegrityException], [NoPeerAvailableException], or any
  /// underlying I/O error if the download can't finish.
  Future<PieceDownloadResult> run() async {
    await cacheDir.create(recursive: true);

    try {
      // 1. Resume any persisted state.
      final meta = await _loadMeta();
      if (meta != null) {
        _totalSize = meta['total_size'] as int;
        _numPieces = _pieceCountFor(_totalSize);
        await _openPartialAndBitmap();
      }

      // 2. Seed source pool: caller-provided + DHT.
      _sources.addAll(extraSources);
      if (extraSources.isNotEmpty) {
        // We already have at least one full-node-swarm seed — don't pay the
        // multi-second DHT round-trip on the critical path. Kick findSources
        // off in the BACKGROUND to top up the pool with DHT seeders as they
        // reply (_mergeIntoSources de-dups against what we already have) and
        // start the workers immediately on the seeded peer(s). The worker
        // loop also re-queries the DHT on its own when the live pool empties,
        // so a background top-up that lands late is still useful.
        unawaited(() async {
          try {
            final dhtSources =
                await SwarmRegistry.instance.findSources(contentHash);
            _mergeIntoSources(dhtSources);
          } catch (_) { /* DHT down — seeded peers carry the download */ }
        }());
      } else {
        // No seeds at all — the DHT IS the source list, so we must wait for it
        // before we have anything to download from.
        try {
          final dhtSources =
              await SwarmRegistry.instance.findSources(contentHash);
          _mergeIntoSources(dhtSources);
        } catch (_) { /* DHT down — _sources stays empty → NoPeer below */ }
      }

      if (_sources.isEmpty) {
        throw NoPeerAvailableException(contentHash);
      }

      // 3. Probe first piece if totalSize unknown. Walk every source so
      //    a single dead peer doesn't fail the whole download — the
      //    workers won't run until totalSize is set, so this round is
      //    sequential by necessity.
      if (_totalSize == 0) {
        Object? probeError;
        for (final s in _sources) {
          if (_bannedPeers.contains(_keyFor(s))) continue;
          try {
            final probe = await _fetchPiece(
                s, 0, math.min(config.pieceSize, 1 << 24));
            _totalSize = probe.totalSize;
            _numPieces = _pieceCountFor(_totalSize);
            await _openPartialAndBitmap();
            await _writePiece(probe);
            await _writeMeta();
            probeError = null;
            break;
          } on _PeerStallException {
            _markPeerError(s);
            probeError = StateError('stall');
          } on _PeerProtocolException {
            _bannedPeers.add(_keyFor(s));
            probeError = StateError('protocol');
          } on RatsRpcException catch (e) {
            if (e.status == 'unknown_type') {
              _bannedPeers.add(_keyFor(s));
            } else {
              _markPeerError(s);
            }
            probeError = e;
          } catch (e) {
            probeError = e;
          }
        }
        if (_totalSize == 0) {
          throw NoPeerAvailableException(contentHash);
        }
        // Use probeError only to suppress the lint about the assignment.
        // ignore: unused_local_variable
        final _ = probeError;
      }

      if (_numPieces == 0) {
        throw StateError('empty file totalSize=$_totalSize');
      }
      _emitProgress();

      // 4. Workers. Wait for them all to settle, but if any worker
      //    flags _terminalError we cancel the pending RPCs (via the
      //    futures' onError) instead of letting Future.wait sit through
      //    every remaining pieceTimeout. Errors are absorbed inside the
      //    worker loop; Future.wait never sees them so eagerError stays
      //    false.
      // Decoupled from the source count: with a single peer (e.g. a relayed
      // download through the mini-node) we still want maxWorkers piece requests
      // in flight sharing that one peer, not one serial worker paying the full
      // relay round-trip per piece. Capped at the piece count so a tiny file
      // doesn't spin up idle workers.
      final workerCount =
          math.min(config.maxWorkers, math.max(1, _numPieces));
      final workers = <Future<void>>[
        for (int i = 0; i < workerCount; ++i) _worker(),
      ];
      await Future.wait(workers);
    } finally {
      await _partialHandle?.close();
      await _bitmapHandle?.close();
      _partialHandle = null;
      _bitmapHandle  = null;
    }

    if (_terminalError != null) throw _terminalError!;

    // 5. Integrity check.
    final actual = await _sha256OfFile(File(_partialPath));
    if (actual != contentHash.toLowerCase()) {
      await _safeDelete(_partialPath);
      await _safeDelete(_bitmapPath);
      await _safeDelete(_metaPath);
      throw IntegrityException(contentHash, actual);
    }

    // 6. Atomic rename → final cache path.
    await File(_partialPath).rename(finalCachePath);
    await _safeDelete(_bitmapPath);
    await _safeDelete(_metaPath);
    return PieceDownloadResult(finalCachePath, _totalSize);
  }

  // ---- Worker loop ----------------------------------------------------

  Future<void> _worker() async {
    int idleBackoffMs = 80;
    while (true) {
      if (_terminalError != null) return;
      if (_isComplete())          return;

      final piece = _claimNextPiece();
      if (piece == null) {
        // No piece to claim — the other N-1 workers hold them all.
        // Back off exponentially up to 500 ms so the late-game spin
        // (where 3 of 4 workers wait on the last piece) doesn't burn
        // CPU. Reset the moment we hand back to the claim path.
        await Future<void>.delayed(Duration(milliseconds: idleBackoffMs));
        if (idleBackoffMs < 500) idleBackoffMs = (idleBackoffMs * 2).clamp(80, 500);
        if (_terminalError != null) return;
        continue;
      }
      idleBackoffMs = 80;

      final source = _nextSource();
      if (source == null) {
        _releasePiece(piece);
        // (#8) Distinguish "all sources permanently banned" (fail) from "the
        // only sources left are mid-cooldown" (wait — they'll come back). A
        // cooled-down peer is NOT in _bannedPeers, so the ban check below sees
        // it as still-usable and we DON'T fail terminal; instead we back off
        // briefly and retry once the stall cooldown elapses.
        final unbanned =
            _sources.where((s) => !_bannedPeers.contains(_keyFor(s)));
        if (unbanned.isEmpty) {
          // Refill from DHT once before bailing.
          try {
            final fresh = await SwarmRegistry.instance.findSources(contentHash);
            _mergeIntoSources(fresh);
          } catch (_) {}
          if (_sources.where((s) => !_bannedPeers.contains(_keyFor(s)))
              .isEmpty) {
            _failTerminal(NoPeerAvailableException(contentHash));
            return;
          }
          continue;
        }
        // Some source exists but is cooling down — wait out the cooldown
        // rather than hammering _nextSource / the DHT in a tight loop.
        await Future<void>.delayed(const Duration(milliseconds: 500));
        if (_terminalError != null) return;
        continue;
      }

      try {
        // Race the fetch against the terminal-error signal so a peer
        // stall doesn't keep this worker waiting the full piece
        // timeout after another worker has already aborted the run.
        final fetch = _fetchPiece(source, piece.offset, piece.length);
        final signal = (_terminalSignal ??= Completer<void>()).future
            .then<_PieceReply?>((_) => null);
        final reply = await Future.any([fetch, signal]);
        if (reply == null) {
          // Terminal error fired before our fetch returned. Drop the
          // claim and exit; the originating worker has set
          // _terminalError, which the run() loop will surface.
          _releasePiece(piece);
          return;
        }
        await _writePiece(reply);
      } on _PeerStallException catch (_) {
        _releasePiece(piece);
        _markPeerError(source);
      } on _PeerProtocolException catch (_) {
        _releasePiece(piece);
        _bannedPeers.add(_keyFor(source));
      } on RatsRpcException catch (e) {
        _releasePiece(piece);
        if (e.status == 'unknown_type') {
          _bannedPeers.add(_keyFor(source));
        } else {
          _markPeerError(source);
        }
      } catch (e) {
        _releasePiece(piece);
        _failTerminal(e);
        return;
      }
    }
  }

  // ---- Piece + peer selection ----------------------------------------

  _PieceSlice? _claimNextPiece() {
    for (int i = 0; i < _numPieces; ++i) {
      if (_bitmap[i] != 0)            continue;
      if (_claimedPieces.contains(i)) continue;
      _claimedPieces.add(i);
      final offset = i * config.pieceSize;
      final length = math.min(config.pieceSize, _totalSize - offset);
      return _PieceSlice(i, offset, length);
    }
    return null;
  }

  void _releasePiece(_PieceSlice p) {
    _claimedPieces.remove(p.index);
  }

  int _rrCursor = 0;
  PeerSource? _nextSource() {
    if (_sources.isEmpty) return null;
    final now = DateTime.now();
    for (int n = 0; n < _sources.length; ++n) {
      final i = (_rrCursor + n) % _sources.length;
      final s = _sources[i];
      final k = _keyFor(s);
      if (_bannedPeers.contains(k)) continue;
      // (#8) Skip a peer still inside its stall cooldown, but DON'T treat it
      // as banned — once the window elapses it's eligible again.
      final cd = _cooldownUntil[k];
      if (cd != null) {
        if (now.isBefore(cd)) continue;
        _cooldownUntil.remove(k);   // cooldown elapsed — usable again
      }
      _rrCursor = (i + 1) % _sources.length;
      return s;
    }
    return null;
  }

  /// (#8) Record a STALL (transient) error against [source]. Distinct from a
  /// protocol violation, which bans directly via `_bannedPeers.add`. A stall
  /// burst should never permanently ban the SOLE relayed source — that fails
  /// an otherwise-recoverable download. So: ban only when OTHER usable sources
  /// remain; if this is the last one, park it on a short cooldown and reset
  /// its error count so it retries after the relay hiccup clears.
  void _markPeerError(PeerSource source) {
    final k = _keyFor(source);
    final cnt = (_perPeerErrors[k] ?? 0) + 1;
    _perPeerErrors[k] = cnt;
    if (cnt < config.perPeerRetryCap) return;

    // Would banning this peer leave us with no usable source at all?
    final otherUsable = _sources.any((s) {
      final ok = _keyFor(s);
      return ok != k && !_bannedPeers.contains(ok);
    });
    if (!otherUsable) {
      // Sole source on a stall burst — cooldown instead of hard ban so the
      // download can resume once the transient condition clears.
      _cooldownUntil[k] = DateTime.now().add(_kStallCooldown);
      _perPeerErrors[k] = 0;   // give it a fresh budget after the cooldown
      return;
    }
    _bannedPeers.add(k);
    // Evict from _sources so the round-robin walker doesn't keep
    // re-encountering dead peers — saves O(N) skipped sources per
    // tick once a peer is dead.
    _sources.removeWhere((s) => _keyFor(s) == k);
    if (_rrCursor >= _sources.length) _rrCursor = 0;
  }

  String _keyFor(PeerSource s) =>
      s.peerId.isNotEmpty ? s.peerId : s.address;

  void _mergeIntoSources(Iterable<PeerSource> extras) {
    final existing = _sources.map(_keyFor).toSet();
    for (final s in extras) {
      final k = _keyFor(s);
      if (k.isEmpty || existing.contains(k)) continue;
      _sources.add(s);
      existing.add(k);
    }
  }

  // ---- Per-piece RPC --------------------------------------------------

  // ---- Adaptive relay congestion window (audit #1 + #4) --------------------
  // This is NOT a throughput limit. Throughput is set by the relay link's rate,
  // not by how many audio.piece_get are outstanding — extra in-flight requests
  // don't transfer faster, they just pile into the relay's FIFO and overrun it,
  // timing out the tail AND every control RPC stuck behind it (the congestion
  // collapse behind "download messes up the connection" / bidirectional
  // timeouts). So we run a TCP-style congestion window over in-flight pieces
  // (shared across ALL downloads): GROW it to keep the relay pipe saturated for
  // MAX throughput, and only back off when the relay actually signals congestion
  // (a timeout). A 100 MB transfer then rides the link at full speed and never
  // collapses, because it stops just short of overrunning. The window's ceiling
  // scales with mini-node count, so adding mini-nodes raises aggregate
  // throughput rather than just deepening one queue.
  static double _cwnd = 12.0;            // congestion window (pieces in flight)
  // Floor of 1 (was 4): on a constrained uplink a floor of 4 (=1 MB in flight)
  // keeps the link perpetually saturated, so piece replies never arrive within
  // the timeout and get re-requested while still in flight — a double-serve loop
  // that becomes ~95x over-fetch (344 MB to move a 3.5 MB mp3). Allowing the
  // window to settle to a single in-flight piece lets the link actually drain so
  // replies arrive and the loop converges; it grows straight back on a fast link.
  static const double _cwndMin = 1.0;
  static int _relayInFlight = 0;
  static final List<Completer<void>> _relayWaiters = <Completer<void>>[];
  static double _cwndMax() =>
      (RatsClient.instance.knownMiniNodes.length.clamp(1, 16) * 24).toDouble();
  static void _cwndOnSuccess() {  // additive increase (~+1 per window of acks)
    _cwnd = (_cwnd + 1.0 / _cwnd).clamp(_cwndMin, _cwndMax());
  }
  static void _cwndOnCongestion() {  // multiplicative decrease
    _cwnd = (_cwnd / 2).clamp(_cwndMin, _cwndMax());
  }
  static Future<void> _acquireRelaySlot() async {
    while (_relayInFlight >= _cwnd.floor()) {
      final c = Completer<void>();
      _relayWaiters.add(c);
      await c.future;
    }
    _relayInFlight++;
  }
  static void _releaseRelaySlot() {
    if (_relayInFlight > 0) _relayInFlight--;
    if (_relayWaiters.isNotEmpty) _relayWaiters.removeAt(0).complete();
  }

  Future<_PieceReply> _fetchPiece(
      PeerSource source, int offset, int length) async {
    // (DHT un-nerf P0b) Resolve a routable peer_id for a DHT-discovered
    // source that only carries a host:port. Without this the raw address was
    // shoved into request()'s peer_id slot — never in validatedPeerIds, never
    // dialed — so DHT sources threw send_failed and could not serve a byte.
    // Dial once, cache the resolved id per address, ban an address that never
    // validates so we don't redial it for every piece.
    String target = source.peerId;
    if (target.isEmpty) {
      final addr = source.address;
      if (addr.isEmpty || _dhtBanned.contains(addr)) {
        throw _PeerProtocolException('unroutable DHT source $addr');
      }
      final cached = _dhtResolved[addr];
      if (cached != null) {
        target = cached;
      } else {
        final colon = addr.lastIndexOf(':');   // lastIndexOf for IPv6 literals
        final host  = colon > 0 ? addr.substring(0, colon) : '';
        final port  = colon > 0 ? (int.tryParse(addr.substring(colon + 1)) ?? 0) : 0;
        final resolved = await RatsClient.instance.connectAndResolve(host, port);
        if (resolved == null) {
          _dhtBanned.add(addr);
          throw _PeerStallException();   // move on to the next source
        }
        _dhtResolved[addr] = resolved;
        target = resolved;
      }
    }
    await _acquireRelaySlot();
    Object? reply;
    try {
      reply = await RatsClient.instance.request(
        target,
        'audio.piece_get',
        {
          'v':            1,
          'content_hash': contentHash,
          'offset':       offset,
          'length':       length,
        },
        timeout: config.pieceTimeout,
      );
      _cwndOnSuccess();        // additive increase — the pipe had room, grow it
    } on RatsRpcException catch (e) {
      if (e.status == 'timeout') {
        _cwndOnCongestion();   // relay congested (or peer slow) — back the window off
        throw _PeerStallException();
      }
      rethrow;
    } finally {
      _releaseRelaySlot();
    }
    if (reply is! Map) {
      throw _PeerProtocolException('non-map reply');
    }
    final m = reply.cast<String, dynamic>();
    final status = m['status'] as String? ?? '';
    if (status != 'ok') {
      throw _PeerProtocolException('status=$status');
    }
    final replyOffset = (m['offset'] as num?)?.toInt() ?? -1;
    if (replyOffset != offset) {
      throw _PeerProtocolException(
          'offset mismatch: asked $offset got $replyOffset');
    }
    final dataB64 = m['data_b64'] as String? ?? '';
    if (dataB64.isEmpty) {
      throw _PeerProtocolException('empty data_b64');
    }
    final bytes = base64Decode(dataB64);
    // Reject any peer whose payload length doesn't match what we asked
    // for. Without this check, a too-long reply would scribble over
    // adjacent pieces already fetched correctly from other peers — the
    // whole-file SHA-256 at the end would catch it, but only after the
    // download is "complete" and after good bytes have been clobbered.
    // Treat it as a protocol violation so the peer is banned and the
    // piece is released for retry against a different source.
    if (bytes.length != length) {
      throw _PeerProtocolException(
          'length mismatch: asked $length got ${bytes.length}');
    }
    final total = (m['total_size'] as num?)?.toInt() ?? 0;
    return _PieceReply(
        offset:    offset,
        bytes:     bytes,
        totalSize: total > 0 ? total : _totalSize);
  }

  // ---- Persistence ----------------------------------------------------

  Future<void> _openPartialAndBitmap() async {
    if (_totalSize <= 0) {
      throw StateError('_openPartialAndBitmap before totalSize known');
    }
    if (_numPieces == 0) _numPieces = _pieceCountFor(_totalSize);

    final partial = File(_partialPath);
    final bitmap  = File(_bitmapPath);

    // Ensure partial exists at the right size. FileMode.write creates +
    // truncates, so do that only when missing.
    if (!await partial.exists()) {
      final raf = await partial.open(mode: FileMode.write);
      await raf.truncate(_totalSize);
      await raf.close();
    } else {
      final len = await partial.length();
      if (len < _totalSize) {
        final raf = await partial.open(mode: FileMode.append);
        await raf.truncate(_totalSize);
        await raf.close();
      }
    }
    // Open r/w handle. FileMode.append on Dart's RandomAccessFile
    // permits both setPosition + writeFrom (verified across Win/Linux/
    // macOS). Read isn't needed because we'll re-open for SHA-256.
    _partialHandle = await partial.open(mode: FileMode.append);

    if (!await bitmap.exists() ||
        (await bitmap.length()) != _numPieces) {
      _bitmap = Uint8List(_numPieces);
      await bitmap.writeAsBytes(_bitmap, flush: true);
    } else {
      _bitmap = Uint8List.fromList(await bitmap.readAsBytes());
    }
    _bitmapHandle = await bitmap.open(mode: FileMode.append);

    _gotBytes = 0;
    for (int i = 0; i < _numPieces; ++i) {
      if (_bitmap[i] == 0) continue;
      final offset = i * config.pieceSize;
      _gotBytes += math.min(config.pieceSize, _totalSize - offset);
    }
  }

  Future<Map<String, dynamic>?> _loadMeta() async {
    final meta = File(_metaPath);
    if (!await meta.exists()) return null;
    try {
      final raw = await meta.readAsString();
      final parsed = json.decode(raw) as Map<String, dynamic>;
      if (parsed['v'] != 1)                          return null;
      if (parsed['content_hash'] != contentHash)     return null;
      if (parsed['piece_size'] != config.pieceSize)  return null;
      if (parsed['total_size'] is! int)              return null;
      return parsed;
    } catch (_) {
      return null;
    }
  }

  Future<void> _writeMeta() async {
    final m = {
      'v':             1,
      'content_hash':  contentHash,
      'piece_size':    config.pieceSize,
      'total_size':    _totalSize,
      'started_at_ms': DateTime.now().millisecondsSinceEpoch,
    };
    await File(_metaPath).writeAsString(json.encode(m), flush: true);
  }

  Future<void> _writePiece(_PieceReply reply) async {
    final pieceIndex = reply.offset ~/ config.pieceSize;
    if (pieceIndex < 0 || pieceIndex >= _numPieces) return;

    await _ioMutex.run(() async {
      if (_bitmap[pieceIndex] != 0) return; // race winner already wrote
      final partial = _partialHandle;
      final bitmap  = _bitmapHandle;
      if (partial == null || bitmap == null) {
        throw StateError('handles not open during _writePiece');
      }
      // Bytes BEFORE bitmap: if a crash interrupts us, the bitmap can
      // never claim a piece is present when the bytes aren't. The
      // reverse (bytes written, bitmap not yet flipped) just means
      // we'll re-download that piece on resume — correct + safe.
      await partial.setPosition(reply.offset);
      await partial.writeFrom(reply.bytes);
      await partial.flush();
      await bitmap.setPosition(pieceIndex);
      await bitmap.writeByte(1);
      await bitmap.flush();
      _bitmap[pieceIndex] = 1;
      _gotBytes += reply.bytes.length;
    });
    _claimedPieces.remove(pieceIndex);
    _emitProgress();
  }

  bool _isComplete() {
    if (_numPieces == 0) return false;
    for (int i = 0; i < _numPieces; ++i) {
      if (_bitmap[i] == 0) return false;
    }
    return true;
  }

  int _pieceCountFor(int total) {
    if (total <= 0) return 0;
    return (total + config.pieceSize - 1) ~/ config.pieceSize;
  }

  void _failTerminal(Object error) {
    if (_terminalError != null) return;
    _terminalError = error;
    // Unblock any worker mid-RPC so we don't sit through pieceTimeout
    // before the run() future resolves.
    final s = _terminalSignal;
    if (s != null && !s.isCompleted) s.complete();
  }

  void _emitProgress() {
    if (onProgress != null) onProgress!(_gotBytes, _totalSize);
  }

  Future<String> _sha256OfFile(File f) async {
    // Stream the file through chunked SHA-256 so memory stays bounded
    // regardless of file size. Audio is small today (a few MB), but the
    // PieceDownloader has no opinion about file type — keeping the
    // hash logic byte-size-independent means a future use for video /
    // sample packs doesn't ambush us with an OOM.
    final raf = await f.open();
    try {
      final captured = <crypto.Digest>[];
      final out = _DigestSink(captured);
      final input = crypto.sha256.startChunkedConversion(out);
      const block = 64 * 1024;
      while (true) {
        final b = await raf.read(block);
        if (b.isEmpty) break;
        input.add(b);
      }
      input.close();
      if (captured.isEmpty) {
        throw StateError('SHA-256 chunked conversion produced no digest');
      }
      final digest = captured.single;
      final sb = StringBuffer();
      for (final b in digest.bytes) {
        sb.write(b.toRadixString(16).padLeft(2, '0'));
      }
      return sb.toString();
    } finally {
      await raf.close();
    }
  }

  Future<void> _safeDelete(String path) async {
    try {
      final f = File(path);
      if (await f.exists()) await f.delete();
    } catch (_) {}
  }
}

class _PieceSlice {
  _PieceSlice(this.index, this.offset, this.length);
  final int index;
  final int offset;
  final int length;
}

class _PieceReply {
  _PieceReply({required this.offset, required this.bytes, required this.totalSize});
  final int       offset;
  final Uint8List bytes;
  final int       totalSize;
}

class _PeerStallException implements Exception {}
class _PeerProtocolException implements Exception {
  _PeerProtocolException(this.reason);
  final String reason;
}

/// Sink that captures the single digest emitted by
/// `crypto.sha256.startChunkedConversion`. We don't depend on
/// `package:async`'s AccumulatorSink so SHA-256 stays a no-extra-dep
/// piece of the downloader.
class _DigestSink implements Sink<crypto.Digest> {
  _DigestSink(this._out);
  final List<crypto.Digest> _out;
  @override void add(crypto.Digest d) => _out.add(d);
  @override void close() {}
}

/// Minimal async mutex so concurrent workers can't interleave their
/// setPosition+write+flush on the shared RandomAccessFile.
class _Mutex {
  Completer<void>? _busy;

  Future<T> run<T>(Future<T> Function() body) async {
    while (_busy != null) {
      await _busy!.future;
    }
    final tag = Completer<void>();
    _busy = tag;
    try {
      return await body();
    } finally {
      _busy = null;
      tag.complete();
    }
  }
}
