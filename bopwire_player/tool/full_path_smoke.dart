// full_path_smoke.dart — exercises the entire UDP/443-only network:
//   1. RatsClient.initialize → connects to the VPS mini-node over QUIC/443
//   2. publicAddress populated via the new stun.observe verb
//   3. routes.get from the VPS, picks the freshest full node
//   4. Reachability-driven setRelayVia (mirrors LibratsDiscovery)
//   5. Round-trip a few rats verbs (status, songs.list)
//   6. Round-trip the same calls via the new `h3.request` verb to confirm
//      HTTP-shaped traffic also tunnels through librats on UDP/443.

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:bopwire_player/src/services/rats_client.dart';

void log(String msg) { /* ignore: avoid_print */ print(msg); }

Future<void> main() async {
  log('=== full_path_smoke ===');
  final rats = await RatsClient.initialize();
  log('  own peer id : ${rats.ownPeerId}');

  // Wait for VPS handshake AND for the unawaited stun.observe to come back.
  for (int i = 0; i < 20 && rats.publicAddress.isEmpty; ++i) {
    await Future.delayed(const Duration(seconds: 1));
  }
  log('  public addr : ${rats.publicAddress}');
  if (rats.publicAddress.isEmpty) {
    log('  FAIL: stun.observe never populated publicAddress');
    rats.dispose(); exit(1);
  }
  if (rats.validatedPeerIds.isEmpty) {
    log('  FAIL: no VPS peer validated');
    rats.dispose(); exit(2);
  }
  final vps = rats.validatedPeerIds.first;

  log('-> routes.get');
  final routes = await rats.requestRoutes();
  log('   ${routes.length} routes');
  Map<String, dynamic>? home;
  int best = -1;
  for (final r in routes) {
    final pid   = r['rats_peer_id']   as String? ?? '';
    final reach = r['reachability']   as String? ?? 'unknown';
    final pub   = r['public_address'] as String? ?? '';
    final seen  = r['last_seen_ms']   as int?    ?? 0;
    log('     $pid  reach=$reach  pub=$pub  seen=$seen');
    if (pid.isEmpty || pid == rats.ownPeerId) continue;
    final canDirect = reach == 'direct' && pub.isNotEmpty;
    rats.setRelayVia(pid, canDirect ? null : vps);
    if (seen > best) { best = seen; home = r; }
  }
  if (home == null) {
    log('  FAIL: no full node in routes');
    rats.dispose(); exit(3);
  }
  final homePid = home['rats_peer_id'] as String;

  Future<void> verb(String type, Map<String, dynamic> body) async {
    log('-> $type');
    try {
      final reply = await rats.request(homePid, type, body,
          timeout: const Duration(seconds: 12));
      var s = jsonEncode(reply);
      if (s.length > 240) s = '${s.substring(0, 240)}...';
      log('   $s');
    } catch (e) {
      log('   FAIL: $e');
    }
  }

  log('--- structured librats verbs ---');
  await verb('status',     const {});
  await verb('songs.list', const {});

  log('--- HTTP-shaped tunnel via h3.request ---');
  await verb('h3.request', const {'method': 'GET', 'path': '/status', 'body': ''});
  await verb('h3.request', const {'method': 'GET', 'path': '/songs',  'body': ''});

  rats.dispose(); exit(0);
}
