// playback_smoke.dart — exercises the player's audio streaming path end-
// to-end against the live VPS + home node from a Dart host. Mirrors the
// RatsClient code paths the Flutter player uses, so any fix here lands
// in the player automatically (both call into lib/src/services/rats_client.dart).
//
// Flow:
//   1. RatsClient.initialize → connect to VPS
//   2. routes.get → pick the home node + set relay (mini-node) for it
//   3. songs.list → grab the first song's content_hash
//   4. requestAudio(home, hash) → assemble bytes via the binary channel
//   5. Report total bytes, first-byte latency, throughput.
//
// Run with: dart run tool/playback_smoke.dart

import 'dart:async';
import 'dart:io';

import 'package:bopwire_player/src/services/rats_client.dart';

void log(String msg) {
  // ignore: avoid_print
  print(msg);
}

Future<void> main() async {
  log('=== playback_smoke ===\n');

  log('step 1 — RatsClient.initialize');
  final rats = await RatsClient.initialize();
  log('  own peer  : ${rats.ownPeerId}');
  for (int i = 0; i < 20 && rats.validatedPeerIds.isEmpty; ++i) {
    await Future.delayed(const Duration(seconds: 1));
  }
  if (rats.validatedPeerIds.isEmpty) {
    log('FAIL: VPS handshake never completed');
    rats.dispose();
    exit(2);
  }
  final vps = rats.validatedPeerIds.first;
  log('  VPS peer  : $vps\n');

  log('step 2 — routes.get');
  final routes = await rats.requestRoutes(timeout: const Duration(seconds: 10));
  log('  ${routes.length} routes from VPS');
  Map<String, dynamic>? pick;
  for (final r in routes) {
    final pid = r['rats_peer_id'] as String? ?? '';
    if (pid.isEmpty || pid == rats.ownPeerId) continue;
    pick = r;
    break;
  }
  if (pick == null) {
    log('FAIL: no home node in route table');
    rats.dispose();
    exit(3);
  }
  final home  = pick['rats_peer_id']   as String;
  final pub   = pick['public_address'] as String? ?? '';
  final reach = pick['reachability']   as String? ?? 'unknown';
  log('  home peer : $home');
  log('  home addr : "$pub"  reach=$reach');

  // Mirror LibratsDiscovery.refresh — relay everything through the VPS
  // for nodes the mini-node tagged "relay".
  if (reach == 'direct' && pub.isNotEmpty) {
    rats.setRelayVia(home, null);
    log('  routing   : direct (public address known)');
  } else {
    rats.setRelayVia(home, vps);
    log('  routing   : via VPS mini-node ($vps)');
  }
  log('');

  log('step 3 — songs.list');
  final songsBody = await rats.request(home, 'songs.list', const {},
      timeout: const Duration(seconds: 15));
  final songsList = (songsBody as List?) ?? const [];
  if (songsList.isEmpty) {
    log('FAIL: home library is empty');
    rats.dispose();
    exit(4);
  }
  Map<String, dynamic>? song;
  for (final s in songsList) {
    final m = (s as Map?)?.cast<String, dynamic>() ?? const {};
    if ((m['content_hash'] as String? ?? '').isNotEmpty) { song = m; break; }
  }
  if (song == null) {
    log('FAIL: no song has a content_hash');
    rats.dispose();
    exit(5);
  }
  final hash  = song['content_hash'] as String;
  final title = song['title']        as String? ?? '';
  log('  picking   : "$title"  hash=${hash.substring(0, 12)}...\n');

  log('step 4 — requestAudio (stream.open + binary chunks)');
  final sw = Stopwatch()..start();
  try {
    final bytes = await rats.requestAudio(home, hash,
        openTimeout:  const Duration(seconds: 10),
        totalTimeout: const Duration(seconds: 60));
    sw.stop();
    final secs    = sw.elapsedMilliseconds / 1000.0;
    final mbps    = bytes.length / secs / 1024 / 1024;
    log('  OK: received ${bytes.length} bytes in '
        '${sw.elapsedMilliseconds}ms (${mbps.toStringAsFixed(2)} MB/s)');

    // Quick sanity check: ogg starts with "OggS", mp3 with ID3 or 0xFF 0xFB.
    if (bytes.length >= 4) {
      final magic = String.fromCharCodes(bytes.sublist(0, 4));
      log('  first 4   : "$magic" (0x'
          '${bytes.sublist(0, 4).map((b) => b.toRadixString(16).padLeft(2, "0")).join()})');
    }
  } catch (e) {
    sw.stop();
    log('FAIL after ${sw.elapsedMilliseconds}ms: $e');
    rats.dispose();
    exit(6);
  }

  rats.dispose();
  log('\nOK');
  exit(0);
}
