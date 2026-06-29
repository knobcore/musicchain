// PlayerServer wires RatsClient.onRequest to handle peer-to-peer verbs
// other players (or the full node steering them) call against us:
//
//   stream.open(content_hash)  — legacy single-peer streaming: if the file
//                                is in our local library, allocate a stream
//                                id, reply, and stream chunks back via
//                                librats binary sends. Kept for backward
//                                compatibility with older players that
//                                don't speak audio.piece_get yet.
//
//   audio.piece_get(content_hash, offset, length)
//                               — synchronous byte range from a held file.
//                                 PieceDownloader fans this verb out across
//                                 many peers in parallel and reassembles in
//                                 order, giving resumable, multi-source
//                                 downloads on top of librats's vanilla
//                                 messaging channel. Length is capped at
//                                 kMaxPieceBytes so a malicious caller can't
//                                 ask us for the whole file in one breath.
//
//   library.list()              — return our entire local library so
//                                another wallet can browse what we have
//                                (off-chain).

import 'dart:async';
import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import '../ffi/rats_bindings.dart' as ffi;
import 'library_service.dart';
import 'rats_client.dart';

class PlayerServer {
  PlayerServer._(this._lib, this._bindings, this._handle);

  static PlayerServer? _instance;
  static Future<PlayerServer> initialize() async {
    if (_instance != null) return _instance!;
    final rats = RatsClient.instance;
    final lib  = LibraryService.instance;
    await lib.ensureLoaded();
    final inst = PlayerServer._(lib, rats.bindingsForServer,
                                rats.handleForServer);
    // Chain our dispatcher behind any previously-installed handler so
    // multiple subsystems can register without clobbering each other.
    // We're first in the chain; the prior handler (if any) becomes the
    // fallback for verbs we don't know.
    final prior = rats.onRequest;
    rats.onRequest = (peerId, type, body, {originator = ''}) async {
      final mine = await inst._dispatch(
          peerId, type, body, originator: originator);
      if (mine != null) return mine;
      if (prior != null) {
        return prior(peerId, type, body, originator: originator);
      }
      return null;
    };
    _instance = inst;
    return inst;
  }

  static PlayerServer get instance {
    final i = _instance;
    if (i == null) throw StateError('PlayerServer.initialize() not called');
    return i;
  }

  final LibraryService    _lib;
  final ffi.RatsBindings  _bindings;
  final Pointer<Void>     _handle;
  final _rng = Random.secure();

  // Active stream registry. We need this for two reasons:
  //   1. Mid-stream cancellation: if the receiving peer disconnects (or
  //      we otherwise notice it's pointless to keep sending), flipping
  //      `cancelled` lets the in-flight `_streamChunks` loop bail out
  //      between chunks instead of blasting kilobytes at a dead socket.
  //   2. Cleanup correlation: keying by streamId lets a future ACK /
  //      flow-control message find the right RandomAccessFile to close.
  final Map<int, _ActiveStream> _streams = {};

  static const int chunkPayload = 16 * 1024;

  // Backpressure knobs. We don't have application-level ACKs yet, so we
  // do TIME-based pacing: after every kPaceEveryChunks chunks we sleep
  // for kPaceDelay. With 16 KB payloads, 4 chunks per 8 ms works out to
  // ~8 MB/s per stream — well under the 5 MB/s target we want to leave
  // for a busy mini-node to relay everyone else's traffic too, but
  // still bursty enough that TCP gets to coalesce sends. If we ever
  // wire up real receiver ACKs, this becomes a sliding window instead.
  static const int      kPaceEveryChunks = 4;
  static const Duration kPaceDelay       = Duration(milliseconds: 8);

  /// PieceDownloader requests at most 256 KB per call; we cap at 512 KB
  /// so a small protocol-version mismatch doesn't let a peer pull
  /// the whole file in one go. Replies are base64-encoded in JSON, so
  /// the on-wire size is ~33% larger than the byte length.
  static const int kMaxPieceBytes = 512 * 1024;

  Future<Map<String, dynamic>?> _dispatch(
      String peerId, String type, Map<String, dynamic> body,
      {String originator = ''}) async {
    switch (type) {
      case 'audio.piece_get': return _handlePieceGet(body);
      case 'stream.open':     return _handleStreamOpen(peerId, body, originator);
      case 'stream.cancel':   return _handleStreamCancel(body);
      case 'library.list':    return _handleLibraryList();
      default: return null;
    }
  }

  /// stream.cancel — the requester skipped/abandoned the track. Stop pushing
  /// immediately instead of streaming the rest of the file through the relay
  /// (which would starve the requester's NEXT stream.open — the "skip → next
  /// track hangs ~30 s" bug). The chunk loop notices `_ActiveStream.cancelled`
  /// next iteration and bails. Idempotent: an unknown/finished stream_id is a
  /// no-op, so duplicate cancels (local stall guard + explicit skip) are safe.
  Future<Map<String, dynamic>?> _handleStreamCancel(
      Map<String, dynamic> body) async {
    final sid = (body['stream_id'] as num?)?.toInt();
    if (sid != null) cancelStream(sid);
    return {'cancelled': sid != null};
  }

  /// audio.piece_get — return a base64-encoded slice of a local file.
  /// Always synchronous in the JSON reply, no out-of-band binary stream.
  Future<Map<String, dynamic>?> _handlePieceGet(
      Map<String, dynamic> body) async {
    final v = body['v'];
    if (v != null && v != 1) {
      return {'v': 1, 'status': 'bad_version', 'error': 'expected v=1, got $v'};
    }
    final hash = body['content_hash'] as String? ?? '';
    if (hash.length != 64) {
      return {'v': 1, 'status': 'bad_hash',
              'error': 'content_hash must be 64-hex SHA-256'};
    }
    final offset = (body['offset'] as num?)?.toInt() ?? 0;
    final length = (body['length'] as num?)?.toInt() ?? 0;
    if (offset < 0 || length <= 0 || length > kMaxPieceBytes) {
      return {'v': 1, 'status': 'bad_range',
              'error': 'offset>=0, 0<length<=$kMaxPieceBytes'};
    }
    final entry = _lib.entryByHash(hash);
    if (entry == null || !entry.isLocal) {
      return {'v': 1, 'status': 'not_held', 'content_hash': hash};
    }
    final file = File(entry.filePath);
    int totalSize;
    Uint8List slice;
    RandomAccessFile? raf;
    try {
      // Open first, then stat through the same handle. Doing it the
      // other way (file.length() then file.open()) opens TWO handles
      // and leaves a window where another process could swap the file
      // underneath us — peers would receive bytes from one file but a
      // total_size from another. With one handle this is impossible.
      raf = await file.open();
      totalSize = await raf.length();
      if (offset >= totalSize) {
        return {'v': 1, 'status': 'bad_range',
                'error': 'offset $offset >= file size $totalSize'};
      }
      final readLen =
          (offset + length > totalSize) ? (totalSize - offset) : length;
      await raf.setPosition(offset);
      slice = await raf.read(readLen);
    } on FileSystemException catch (e) {
      return {'v': 1, 'status': 'io_error', 'error': e.message};
    } finally {
      await raf?.close();
    }
    return {
      'v':            1,
      'status':       'ok',
      'content_hash': hash,
      'offset':       offset,
      'length':       slice.length,
      'total_size':   totalSize,
      'data_b64':     base64Encode(slice),
    };
  }

  Future<Map<String, dynamic>?> _handleStreamOpen(
      String peerId, Map<String, dynamic> body, String originator) async {
    final hash = body['content_hash'] as String? ?? '';
    if (hash.isEmpty) return null;
    // (#13 instability fix) On the relay path the originator peer-id is
    // written verbatim into the F-frame prefix (codeUnitAt(0..39)). A
    // malformed originator (non-empty but not exactly 40 chars) would
    // RangeError mid-stream and hang the seeder ~30s. Reject it here so the
    // requester learns immediately instead of stalling.
    if (originator.isNotEmpty && originator.length != 40) {
      return {'matched': false, 'content_hash': hash, 'error': 'bad_originator'};
    }
    // #10: the requester threads the broker-minted delivery_id here so we
    // can stamp it into the F-frame prefix the mini-node accounts on.
    final deliveryId = body['delivery_id'] as String? ?? '';
    final entry = _lib.entryByHash(hash);
    if (entry == null || !entry.isLocal) {
      return {'matched': false, 'content_hash': hash};
    }
    final file  = File(entry.filePath);
    // Stat through the File handle (one syscall) instead of
    // file.readAsBytes() — which would slurp the whole audio file into
    // a Uint8List, blowing RAM on phones for any track over a few MB
    // and adding I/O latency to the reply envelope. The actual bytes
    // get streamed off disk page-by-page inside _streamChunks.
    final int totalBytes;
    try {
      totalBytes = await file.length();
    } on FileSystemException catch (e) {
      return {'matched': false, 'content_hash': hash, 'error': e.message};
    }
    // (#17 instability fix) The REQUESTER proposes a stream_id it has
    // guaranteed unique among its own concurrent fetches (a monotonic
    // counter), so chunks can't be misrouted in its _streams map by two
    // serving peers independently picking the same random 32-bit id. We
    // honor it for the wire framing, masking to the 32 bits the chunk header
    // actually carries, and only reallocate if it already names another
    // stream WE are serving (cross-requester collision on this node). Absent
    // (old client) → fall back to a random id, the legacy behavior.
    int sid = (body['client_stream_id'] as num?)?.toInt()
        ?? _rng.nextInt(0xFFFFFFFF);
    sid &= 0xFFFFFFFF;
    while (_streams.containsKey(sid)) {
      sid = _rng.nextInt(0xFFFFFFFF);
    }

    // If [originator] is non-empty, the request arrived via VPS relay.
    // The VPS doesn't track stream chunks by req_id — only the 'F' tag
    // + 40-byte hex target — so the chunks have to be prefixed and sent
    // to peerId (the VPS) instead of dropped at the immediate sender.
    // Direct peer-to-peer (originator empty) sends straight to peerId.
    unawaited(_streamChunks(
      sendTo:     peerId,
      relayTo:    originator,
      streamId:   sid,
      file:       file,
      totalBytes: totalBytes,
      deliveryId: deliveryId,
    ));

    return {
      'matched':      true,
      'stream_id':    sid,
      'total_bytes':  totalBytes,
      'chunk_bytes':  chunkPayload,
      'content_type': entry.audioFormat == 'ogg' ? 'audio/ogg' : 'audio/mpeg',
      'source':       originator.isEmpty ? 'peer' : 'peer-relay',
    };
  }

  /// Externally request that an in-flight stream stop sending. The next
  /// chunk-loop iteration will notice [`_ActiveStream.cancelled`] and
  /// bail out, closing the RandomAccessFile and unregistering itself.
  /// Idempotent: calling on an unknown / already-finished stream is a
  /// no-op so disconnect handlers can fire freely.
  void cancelStream(int streamId) {
    final active = _streams[streamId];
    if (active == null) return;
    active.cancelled = true;
  }

  /// Cancel every stream we're currently sending to [peerId]. Intended
  /// to be wired into RatsClient.onPeerDisconnected once a clean lookup
  /// from peerId → stream_id exists at the RatsClient layer.
  /// TODO: rats_client.dart is off-limits in this change set, so the
  /// disconnect hook isn't wired yet. The API is exposed here so a
  /// future patch can call it without further surgery on this file.
  void cancelStreamsForPeer(String peerId) {
    final victims = <int>[];
    _streams.forEach((sid, active) {
      if (active.peerId == peerId) victims.add(sid);
    });
    for (final sid in victims) {
      cancelStream(sid);
    }
  }

  Future<void> _streamChunks({
    required String sendTo,
    required String relayTo,
    required int    streamId,
    required File   file,
    required int    totalBytes,
    String          deliveryId = '',
  }) async {
    final relay      = relayTo.isNotEmpty;
    const kTagF      = 0x46; // 'F'
    // #10: on the relay path the F-frame prefix is ALWAYS 'F'(1) +
    // target(40) + delivery_id(16). The mini-node strips exactly 1+40+16
    // (tools/mini_node.cpp on_relay_binary), so we always reserve the 16
    // bytes — writing the delivery_id when we have one, zeros otherwise —
    // or the receiver would read the chunk header off the delivery_id.
    final prefixLen  = relay ? (1 + 40 + 16) : 0;
    final peerPtr    = sendTo.toNativeUtf8();
    final native     = malloc<Uint8>(prefixLen + 9 + chunkPayload);
    // Typed-data view over the same native allocation so we can bulk-copy each
    // disk slice in with setRange (one memmove) instead of a per-byte Dart
    // loop — on a hot stream that's the difference between ~16K bounds-checked
    // index writes per chunk and a single native block copy.
    final nativeView = native.asTypedList(prefixLen + 9 + chunkPayload);
    // Page-sized scratch we reuse across iterations. We could read
    // directly into the native buffer with a typed-data view, but
    // RandomAccessFile.read returns a fresh Uint8List anyway, so the
    // copy cost is unavoidable; reusing 'native' just keeps malloc
    // churn out of the hot loop.
    RandomAccessFile? raf;
    final active = _ActiveStream(peerId: sendTo);
    _streams[streamId] = active;
    try {
      raf = await file.open();
      if (relay) {
        native[0] = kTagF;
        // 40 hex characters of the originator's peer id, ASCII-encoded.
        for (int i = 0; i < 40; ++i) {
          native[1 + i] = relayTo.codeUnitAt(i);
        }
        // 16 raw delivery_id bytes (#10). 32 valid hex chars → 16 bytes;
        // otherwise zeros (no triangulation for this delivery).
        final haveDid = deliveryId.length == 32;
        for (int i = 0; i < 16; ++i) {
          int v = 0;
          if (haveDid) {
            v = (_hexNibble(deliveryId.codeUnitAt(i * 2)) << 4) |
                 _hexNibble(deliveryId.codeUnitAt(i * 2 + 1));
          }
          native[1 + 40 + i] = v;
        }
      }
      var offset = 0;
      var seq    = 0;
      while (offset < totalBytes) {
        // Cancellation gate. We check at the top of the loop so a
        // disconnect that races with the very first chunk still wins;
        // any chunk we've already framed is at most one extra send.
        if (active.cancelled || _streams[streamId] != active) break;

        final n = (offset + chunkPayload > totalBytes)
            ? totalBytes - offset
            : chunkPayload;
        final eof = (offset + n) >= totalBytes;

        // Pull n bytes off disk. read() advances the file position, so
        // we never need to call setPosition() in the loop — sequential
        // reads are the fast path on every platform we ship.
        final slice = await raf.read(n);
        if (slice.length != n) {
          // Truncated read = file changed under us mid-stream. Bail
          // rather than send a short chunk that wouldn't match the
          // total_bytes the receiver was promised.
          break;
        }

        native[prefixLen + 0] = streamId & 0xFF;
        native[prefixLen + 1] = (streamId >> 8) & 0xFF;
        native[prefixLen + 2] = (streamId >> 16) & 0xFF;
        native[prefixLen + 3] = (streamId >> 24) & 0xFF;
        native[prefixLen + 4] = seq & 0xFF;
        native[prefixLen + 5] = (seq >> 8) & 0xFF;
        native[prefixLen + 6] = (seq >> 16) & 0xFF;
        native[prefixLen + 7] = (seq >> 24) & 0xFF;
        native[prefixLen + 8] = eof ? 1 : 0;
        // Bulk copy the payload in one memmove (see nativeView above).
        nativeView.setRange(prefixLen + 9, prefixLen + 9 + n, slice);
        _bindings.sendBinary(_handle, peerPtr,
            native.cast<Void>(), prefixLen + 9 + n);
        offset += n;
        ++seq;

        // Cooperative pacing — but ONLY when more than one stream is in
        // flight. A lone stream has no one to share the mini-node's outbound
        // bandwidth with, so pacing it just adds first-byte latency for no
        // benefit: let it run at line rate. The instant a SECOND stream
        // starts, both pace (every kPaceEveryChunks chunks, kPaceDelay) so
        // neither monopolises the relay and control traffic still interleaves.
        if (_streams.length > 1 && seq % kPaceEveryChunks == 0) {
          await Future<void>.delayed(kPaceDelay);
        }
      }
    } catch (_) {
      // Swallow: we're an unawaited future. Dropping the stream is
      // already the right user-visible outcome; the receiver will see
      // a missing EOF and time out.
    } finally {
      // Always: close the disk handle, free the native buffers, and
      // deregister from _streams. Doing this in finally — not after the
      // loop — means a thrown read() doesn't leak a file descriptor.
      try { await raf?.close(); } catch (_) {}
      malloc.free(native);
      malloc.free(peerPtr);
      if (_streams[streamId] == active) _streams.remove(streamId);
      active.done.complete();
    }
  }

  Future<Map<String, dynamic>?> _handleLibraryList() async {
    final out = <Map<String, dynamic>>[];
    for (final e in _lib.entries) {
      out.add({
        'content_hash':     e.contentHash,
        'fingerprint_hash': e.fingerprintHash,
        'title':            e.title,
        'artist':           e.artist,
        'album':            e.album,
        'genre':            e.genre,
        'duration_ms':      e.durationMs,
        'audio_format':     e.audioFormat,
        'is_local':         e.isLocal,
      });
    }
    return {'songs': out};
  }
}

/// Bookkeeping for one in-flight stream.send loop. Lives in the
/// PlayerServer._streams map, keyed by stream_id. The only mutable
/// field is [cancelled] — the sender loop polls it between chunks; an
/// outside caller (currently cancelStream/cancelStreamsForPeer, later
/// the disconnect hook) sets it. [done] is provided so a future patch
/// can `await` clean teardown when shutting the server down without
/// changing public method signatures.
class _ActiveStream {
  _ActiveStream({required this.peerId});
  final String peerId;
  bool cancelled = false;
  final Completer<void> done = Completer<void>();
}

// One hex char (ASCII code unit) → nibble value 0..15 (0 on non-hex). Used
// to pack the delivery_id hex into the F-frame prefix (#10).
int _hexNibble(int c) {
  if (c >= 0x30 && c <= 0x39) return c - 0x30;        // '0'..'9'
  if (c >= 0x61 && c <= 0x66) return c - 0x61 + 10;   // 'a'..'f'
  if (c >= 0x41 && c <= 0x46) return c - 0x41 + 10;   // 'A'..'F'
  return 0;
}
