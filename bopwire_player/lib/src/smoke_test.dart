// Programmatic smoke test of the player's network stack.
//
// Compiled INTO the player and triggered by `--dart-define=SMOKE_TEST=true`.
// It runs AFTER normal app init (wallet loaded, RatsClient connected,
// discovery + presence up) so it reuses the real identity + live connection,
// then runs three headless actions against the "James Shinra" album held by
// the phone and pulled via the VPS relay:
//
//   ACTION A: download the FULL album (every track)
//   ACTION B: download a single track
//   ACTION C: stream (play) one song headless by draining its bytes
//
// Every line is prefixed `[smoke]` so it is greppable. Each action is wrapped
// in its own try/catch so one failure does not abort the others. The process
// exits with code 0 if all three actions PASS, 1 otherwise.
//
// All APIs used here are the real ones the GUI uses — no BuildContext needed;
// everything is reached through process-wide singletons.

import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'models/song.dart';
import 'services/node_client.dart';
import 'services/node_service.dart';
import 'services/rats_client.dart';

const String _kArtist = 'James Shinra';

void _log(String msg) {
  // ignore: avoid_print
  print('[smoke] $msg');
}

/// Entry point invoked from main() when SMOKE_TEST is set. Never throws —
/// catches everything, prints a PASS/FAIL summary, and exits the process.
Future<void> runSmokeTest() async {
  _log('==================================================');
  _log('SMOKE TEST START (artist filter: "$_kArtist")');
  _log('==================================================');

  bool actionA = false; // full album download
  bool actionB = false; // single track download
  bool actionC = false; // stream one song
  String aDetail = 'not run';
  String bDetail = 'not run';
  String cDetail = 'not run';

  try {
    // -- 1. READINESS -------------------------------------------------------
    final String ratsPeerId = await _awaitReadiness();

    // -- 2. RESOLVE THE ALBUM ----------------------------------------------
    final client = NodeClient(ratsPeerId: ratsPeerId);
    final album = await _resolveAlbum(client);
    if (album.isEmpty) {
      throw StateError('no "$_kArtist" songs resolved from the full node');
    }

    // -- 3. ACTION A: FULL ALBUM DOWNLOAD ----------------------------------
    try {
      final r = await _actionDownloadAlbum(client, album);
      actionA = r.pass;
      aDetail = r.detail;
    } catch (e) {
      aDetail = 'threw: $e';
      _log('ACTION A: FAIL ($aDetail)');
    }

    // -- 4. ACTION B: SINGLE TRACK DOWNLOAD --------------------------------
    try {
      final r = await _actionDownloadSingle(client, album);
      actionB = r.pass;
      bDetail = r.detail;
    } catch (e) {
      bDetail = 'threw: $e';
      _log('ACTION B: FAIL ($bDetail)');
    }

    // -- 5. ACTION C: STREAM ONE SONG --------------------------------------
    try {
      final r = await _actionStream(client, album);
      actionC = r.pass;
      cDetail = r.detail;
    } catch (e) {
      cDetail = 'threw: $e';
      _log('ACTION C: FAIL ($cDetail)');
    }
  } catch (e, st) {
    _log('FATAL before actions could run: $e');
    _log('$st');
  }

  // -- 6. RESULT BLOCK ------------------------------------------------------
  _log('===== SMOKE TEST RESULT =====');
  _log('ACTION A  full album download : ${actionA ? "PASS" : "FAIL"}  | $aDetail');
  _log('ACTION B  single track download: ${actionB ? "PASS" : "FAIL"}  | $bDetail');
  _log('ACTION C  stream one song      : ${actionC ? "PASS" : "FAIL"}  | $cDetail');
  final allPass = actionA && actionB && actionC;
  _log('OVERALL: ${allPass ? "PASS" : "FAIL"}');
  _log('=============================');

  // Headless test: exit so the harness gets a deterministic code. media_kit /
  // the proxy / the rats watchdog timers would otherwise keep the isolate
  // alive forever.
  exit(allPass ? 0 : 1);
}

// ===========================================================================
// 1. READINESS
// ===========================================================================

/// Polls the mutable readiness state off the singletons until all signals are
/// up or the deadline passes. There is NO single "ready" event in the app, so
/// this is a poll-with-deadline loop. Returns the full-node rats peer id once
/// ready; throws on timeout.
Future<String> _awaitReadiness() async {
  const deadline = Duration(seconds: 90);
  final sw = Stopwatch()..start();
  _log('awaiting readiness (timeout ${deadline.inSeconds}s)...');

  RatsClient rats;
  try {
    rats = RatsClient.instance;
  } catch (e) {
    throw StateError('RatsClient not initialized — smoke ran before init: $e');
  }

  String lastReport = '';
  while (true) {
    final validated = rats.validatedPeerIds;
    final hasPeers = validated.isNotEmpty;
    final miniNode = rats.bestMiniNodePeerId ?? rats.firstMiniNodePeerId;
    final hasMini = miniNode != null && miniNode.isNotEmpty;
    // Non-blocking probe of the selected full node id (waitFor: 0 so the loop
    // governs timing, not the pref poller).
    final pid = await NodeService.getRatsPeerId(waitFor: Duration.zero);
    final hasNode = pid.isNotEmpty;

    final report = 'peers=${validated.length} mini=${hasMini ? "yes" : "no"} '
        'node=${hasNode ? "yes" : "no"} (${sw.elapsed.inSeconds}s)';
    if (report != lastReport) {
      _log('readiness: $report');
      lastReport = report;
    }

    if (hasPeers && hasMini && hasNode) {
      _log('readiness: READY — fullNode=${pid.substring(0, pid.length.clamp(0, 12))}… '
          'relay=${miniNode.substring(0, miniNode.length.clamp(0, 12))}…');
      return pid;
    }

    if (sw.elapsed > deadline) {
      throw TimeoutException(
          'readiness not reached within ${deadline.inSeconds}s '
          '($report)');
    }
    await Future<void>.delayed(const Duration(milliseconds: 750));
  }
}

// ===========================================================================
// 2. RESOLVE THE ALBUM
// ===========================================================================

/// Resolves the "James Shinra" album's songs from the full node and confirms
/// each has a live swarm holder (the phone) by checking swarmSize and probing
/// swarm variants. Returns the fetchable songs sorted by track number.
Future<List<Song>> _resolveAlbum(NodeClient client) async {
  _log('resolving album via songs.search artist="$_kArtist"...');

  // Search-by-artist is the fast path; fall back to the full songs.list if it
  // comes back empty (e.g. the node only matches album field).
  List<Song> hits;
  try {
    hits = await client.searchSongsByArtist(_kArtist);
  } catch (e) {
    _log('songs.search threw ($e) — falling back to songs.list');
    hits = await client.getSongs();
  }
  if (hits.isEmpty) {
    _log('songs.search empty — falling back to songs.list');
    hits = await client.getSongs();
  }

  final want = _kArtist.trim().toLowerCase();
  // Match on artist OR album so a self-titled album resolves either way.
  var matched = hits.where((s) {
    final a = s.artist.trim().toLowerCase();
    final al = s.album.trim().toLowerCase();
    return a == want || al == want;
  }).toList();

  // Keep only fetchable rows: valid 64-hex content hash AND an announced
  // swarm holder. (The full node already pre-filters to non-empty swarms, so
  // do not over-filter — if swarmSize>0 hides everything, keep the hash-valid
  // rows as a fallback.)
  List<Song> live = matched
      .where((s) => s.contentHash.length == 64 && s.swarmSize > 0)
      .toList();
  if (live.isEmpty && matched.isNotEmpty) {
    _log('all matched rows have swarmSize<=0 — keeping hash-valid rows as '
        'fallback (node already pre-filters to non-empty swarms)');
    live = matched.where((s) => s.contentHash.length == 64).toList();
  }

  // Dedup by content hash, sort by track number.
  final seen = <String>{};
  final deduped = <Song>[];
  for (final s in live) {
    if (seen.add(s.contentHash)) deduped.add(s);
  }
  deduped.sort((a, b) => a.trackNumber.compareTo(b.trackNumber));

  _log('resolved ${deduped.length} fetchable "$_kArtist" track(s):');
  for (final s in deduped) {
    _log('  #${s.trackNumber} "${s.title}" album="${s.album}" '
        'swarm=${s.swarmSize} hash=${s.contentHash.substring(0, 12)}…');
  }

  // Corroborate at least the first track has a live holder via swarm variants
  // (lookupSwarmVariants swallows errors and returns [] on timeout, so this is
  // informational, not a hard gate).
  if (deduped.isNotEmpty) {
    try {
      final variants = await client.lookupSwarmVariants(deduped.first.contentHash);
      _log('swarm-variant probe for "${deduped.first.title}": '
          '${variants.length} holder(s)'
          '${variants.isNotEmpty ? " — first peer=${variants.first.peerId.substring(0, variants.first.peerId.length.clamp(0, 12))}…" : ""}');
    } catch (e) {
      _log('swarm-variant probe failed (non-fatal): $e');
    }
  }

  return deduped;
}

// ===========================================================================
// 3. ACTION A: FULL ALBUM DOWNLOAD
// ===========================================================================

class _Result {
  _Result(this.pass, this.detail);
  final bool pass;
  final String detail;
}

Future<_Result> _actionDownloadAlbum(
    NodeClient client, List<Song> album) async {
  _log('--- ACTION A: download FULL album (${album.length} track(s)) ---');
  int okCount = 0;
  int totalBytes = 0;
  final albumSw = Stopwatch()..start();

  for (final s in album) {
    final sw = Stopwatch()..start();
    try {
      final path = await client.downloadToLibrary(
        s.contentHash,
        chainSong: s,
        onProgress: (received, total) {
          // Coarse progress: log only at start (received small) to avoid spam.
        },
      );
      sw.stop();
      final f = File(path);
      final bytes = await f.exists() ? await f.length() : 0;
      if (bytes > 0) {
        okCount++;
        totalBytes += bytes;
        final secs = (sw.elapsedMilliseconds / 1000.0).clamp(0.001, 1e9);
        final kbps = bytes / secs / 1024.0;
        _log('  PASS  #${s.trackNumber} "${s.title}": '
            '$bytes B in ${sw.elapsedMilliseconds} ms '
            '= ${kbps.toStringAsFixed(1)} KB/s -> $path');
      } else {
        _log('  FAIL  #${s.trackNumber} "${s.title}": '
            'returned path "$path" but file is empty (0 B)');
      }
    } catch (e) {
      sw.stop();
      final reason = (e is RatsRpcException) ? '${e.status}: ${e.message}' : '$e';
      _log('  FAIL  #${s.trackNumber} "${s.title}": $reason');
    }
  }

  albumSw.stop();
  final secs = (albumSw.elapsedMilliseconds / 1000.0).clamp(0.001, 1e9);
  final mbps = (totalBytes * 8) / (secs * 1e6);
  final detail = '$okCount/${album.length} tracks OK, '
      '$totalBytes B total in ${albumSw.elapsedMilliseconds} ms '
      '= ${mbps.toStringAsFixed(2)} Mbit/s';
  _log('ACTION A summary: $detail');
  // PASS = every track downloaded with non-zero bytes.
  return _Result(okCount == album.length && album.isNotEmpty, detail);
}

// ===========================================================================
// 4. ACTION B: SINGLE TRACK DOWNLOAD
// ===========================================================================

Future<_Result> _actionDownloadSingle(
    NodeClient client, List<Song> album) async {
  // Pick a single track for the isolated download measurement. (Note:
  // downloadToLibrary is idempotent — if ACTION A already cached this track it
  // returns instantly with no network. That is still a valid PASS for the API,
  // and the throughput line will reflect the disk hit.)
  final s = album.first;
  _log('--- ACTION B: download single track "${s.title}" '
      '(${s.contentHash.substring(0, 12)}…) ---');

  final sw = Stopwatch()..start();
  try {
    final path = await client.downloadToLibrary(s.contentHash, chainSong: s);
    sw.stop();
    final f = File(path);
    final bytes = await f.exists() ? await f.length() : 0;
    if (bytes <= 0) {
      final detail = 'returned "$path" but file empty (0 B)';
      _log('ACTION B: FAIL ($detail)');
      return _Result(false, detail);
    }
    final secs = (sw.elapsedMilliseconds / 1000.0).clamp(0.001, 1e9);
    final kbps = bytes / secs / 1024.0;
    final detail = '$bytes B in ${sw.elapsedMilliseconds} ms '
        '= ${kbps.toStringAsFixed(1)} KB/s';
    _log('ACTION B: PASS ($detail) -> $path');
    return _Result(true, detail);
  } catch (e) {
    sw.stop();
    final reason = (e is RatsRpcException) ? '${e.status}: ${e.message}' : '$e';
    _log('ACTION B: FAIL ($reason)');
    return _Result(false, reason);
  }
}

// ===========================================================================
// 5. ACTION C: STREAM ONE SONG (headless)
// ===========================================================================

Future<_Result> _actionStream(NodeClient client, List<Song> album) async {
  final s = album.first;
  _log('--- ACTION C: stream (drain) one song "${s.title}" '
      '(${s.contentHash.substring(0, 12)}…) ---');

  final RatsClient rats = RatsClient.instance;
  // Relay anchor MUST be a mini-node (see node_client RELAY-ANCHOR notes):
  // bestMiniNodePeerId ?? firstMiniNodePeerId. May be null — then request()
  // re-resolves the live relay.
  final vpsPid = rats.bestMiniNodePeerId ?? rats.firstMiniNodePeerId;
  _log('ACTION C: relay anchor = '
      '${vpsPid == null ? "<null, request() re-resolves>" : "${vpsPid.substring(0, vpsPid.length.clamp(0, 12))}…"}');

  final sw = Stopwatch()..start();
  AudioStream? stream;
  try {
    // Replicate fetchAudioToCache's core (without the media_kit proxy) so we
    // measure raw end-to-end streaming from the phone via the VPS relay.
    stream = await rats.streamFromSwarm(
      nodePeerId: client.ratsPeerId!,
      contentHash: s.contentHash,
      vpsPeerId: vpsPid,
    );
  } on RatsRpcException catch (e) {
    sw.stop();
    final detail = 'open failed: ${e.status}: ${e.message}';
    _log('ACTION C: FAIL ($detail)');
    return _Result(false, detail);
  }

  int drained = 0;
  final byteSub = stream.bytes.listen((Uint8List chunk) {
    drained += chunk.length;
  });
  try {
    await stream.done; // completes on full success; rejects on stall/cancel
    sw.stop();
    await byteSub.cancel();
    final secs = (sw.elapsedMilliseconds / 1000.0).clamp(0.001, 1e9);
    final mbps = (drained * 8) / (secs * 1e6);
    final target = stream.totalBytes;
    final complete = target <= 0 || drained == target;
    final detail = '$drained B (header total=$target) in '
        '${sw.elapsedMilliseconds} ms = ${mbps.toStringAsFixed(2)} Mbit/s';
    if (drained > 0 && complete) {
      _log('ACTION C: PASS ($detail)');
      return _Result(true, detail);
    }
    final why = drained <= 0
        ? 'drained 0 bytes'
        : 'short read ($drained/$target)';
    _log('ACTION C: FAIL ($why; $detail)');
    return _Result(false, '$why; $detail');
  } catch (e) {
    sw.stop();
    await byteSub.cancel();
    stream.cancel();
    final detail = 'drain failed: $e (drained $drained/${stream.totalBytes} B)';
    _log('ACTION C: FAIL ($detail)');
    return _Result(false, detail);
  }
}
