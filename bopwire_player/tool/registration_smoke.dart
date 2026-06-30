// registration_smoke.dart — exercises the new no-match fingerprint.submit
// path that registers a song on the chain WITHOUT the player ever uploading
// the audio bytes. The bytes stay with us; the home node just records the
// fingerprint + metadata.
//
// Sequence:
//   1. Open RatsClient, find the home node via routes.get.
//   2. Read the source mp3, compute SHA256 → content_hash, and a dummy
//      "compressed_fingerprint" (sha256 of the file in hex — stands in
//      for the chromaprint blob until Sprint 3d's APK build).
//   3. Submit fingerprint.submit with the metadata. Expect matched=false,
//      registered=true (first time) OR matched=true (re-run).
//   4. Wait for the heartbeat producer to mint a block carrying the song,
//      then verify the song appears in songs.list with the expected hash.

import 'dart:async';
import 'dart:io';

import 'package:crypto/crypto.dart' as crypto;

import 'package:bopwire_player/src/services/rats_client.dart';

void log(String msg) {
  // ignore: avoid_print
  print(msg);
}

const String _filePath =
    r'C:\Users\lain\Documents\Soulseek Downloads\complete\maxspadge1\Surfbort - Reality Star\10 - Reality Star.mp3';

Future<void> main() async {
  log('=== registration_smoke ===\n');

  final rats = await RatsClient.initialize();
  for (int i = 0; i < 20 && rats.validatedPeerIds.isEmpty; ++i) {
    await Future.delayed(const Duration(seconds: 1));
  }
  if (rats.validatedPeerIds.isEmpty) {
    log('FAIL: VPS handshake never completed');
    rats.dispose(); exit(2);
  }
  final vps = rats.validatedPeerIds.first;

  final routes = await rats.requestRoutes(timeout: const Duration(seconds: 10));
  Map<String, dynamic>? pick;
  for (final r in routes) {
    final pid = r['rats_peer_id'] as String? ?? '';
    if (pid.isEmpty || pid == rats.ownPeerId) continue;
    pick = r; break;
  }
  if (pick == null) {
    log('FAIL: no home node in routes');
    rats.dispose(); exit(3);
  }
  final home  = pick['rats_peer_id']   as String;
  final reach = pick['reachability']   as String? ?? 'unknown';
  final pub   = pick['public_address'] as String? ?? '';
  if (reach == 'direct' && pub.isNotEmpty) {
    rats.setRelayVia(home, null);
  } else {
    rats.setRelayVia(home, vps);
  }
  log('  own peer  : ${rats.ownPeerId}');
  log('  home peer : $home  reach=$reach\n');

  log('step 1 — read source file + compute hashes');
  final file = File(_filePath);
  if (!await file.exists()) {
    log('FAIL: source mp3 not at $_filePath');
    rats.dispose(); exit(4);
  }
  final bytes = await file.readAsBytes();
  final contentHash    = crypto.sha256.convert(bytes).toString();
  // Stand-in for the real chromaprint compressed string. The home node
  // hashes this and stores SHA256(fingerprint) as the chain key.
  final dummyFingerprint = 'mc-smoke-fp-$contentHash';
  log('  file_bytes        = ${bytes.length}');
  log('  content_hash      = ${contentHash.substring(0, 16)}...');
  log('  dummy_fingerprint = "${dummyFingerprint.substring(0, 24)}..."\n');

  log('step 2 — fingerprint.submit (NO upload of audio bytes)');
  final r1 = await rats.request(home, 'fingerprint.submit', {
    'fingerprint':   dummyFingerprint,
    'peer_id':       rats.ownPeerId,
    'content_hash':  contentHash,
    'title':         'Reality Star',
    'artist':        'Surfbort',
    'genre':         'rock',
    'album':         'Reality Star',
    'duration_ms':   180000,
    'audio_format':  'mp3',
  }, timeout: const Duration(seconds: 15));
  final m1 = (r1 as Map<String, dynamic>);
  log('  reply: $m1');
  if (m1['matched'] == true) {
    log('  (already registered from a previous run — that is OK)');
  } else if (m1['registered'] != true) {
    log('FAIL: home node refused to register the song');
    rats.dispose(); exit(5);
  } else {
    log('  registered=${m1['registered']}  content_hash matches: '
        '${m1['content_hash'] == contentHash}');
  }

  log('\nstep 3 — wait for the song-bearing block + verify songs.list');
  // The heartbeat producer fires the next block immediately when a
  // registration is queued. Give it up to 30 s.
  bool found = false;
  for (int i = 0; i < 15; ++i) {
    await Future.delayed(const Duration(seconds: 2));
    final list = await rats.request(home, 'songs.list', const {},
        timeout: const Duration(seconds: 10)) as List? ?? const [];
    for (final s in list) {
      final mm = (s as Map?)?.cast<String, dynamic>() ?? const {};
      if (mm['content_hash'] == contentHash) { found = true; break; }
    }
    if (found) break;
  }
  if (!found) {
    log('FAIL: song never appeared in songs.list within 30 s');
    rats.dispose(); exit(6);
  }
  log('  OK: song appears in songs.list with content_hash matching');

  rats.dispose();
  log('\nOK');
  exit(0);
}
