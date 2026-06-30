// rats_smoke.dart — end-to-end test of the deployed network using exactly
// the same discovery + routing logic the player ships in production
// (lib/src/services/librats_discovery.dart). No 127.0.0.1 shortcuts, no
// hand-wired direct-connect. The point is to exercise the real path:
//
//   1. RatsClient.initialize connects this isolate to the VPS mini-node
//      over QUIC/443 (exact same outbound the player makes on startup).
//   2. The mini-node has already probed every storage node that registered
//      with it (a fresh QUIC connection from the mini-node back to the
//      node's STUN-discovered public address from a different source port)
//      and tagged each route either `direct` (inbound reachable) or
//      `relay` (inbound blocked — the mini-node's the only one that can
//      reach it on its already-open flow).
//   3. We pull the route list with routes.get, then for each entry call
//      RatsClient.setRelayVia(node_pid, vps_pid)  when reachability=relay,
//      or clear it when reachability=direct — same as LibratsDiscovery.
//   4. From there on, rats.request() routes automatically: it sends direct
//      to nodes with reachability=direct, and wraps in `relay.forward` to
//      the mini-node for nodes with reachability=relay. The mini-node
//      forwards on its existing QUIC link and rewrites the reply's
//      req_id back transparently.
//   5. We round-trip a few verbs at the auto-selected full node.

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:bopwire_player/src/services/rats_client.dart';

void log(String msg) {
  // ignore: avoid_print
  print(msg);
}

/// Mirrors the body of `LibratsDiscovery.refresh()` minus the Flutter
/// ChangeNotifier / SharedPreferences plumbing — pure routing logic.
Future<Map<String, dynamic>?> runDiscovery(RatsClient rats) async {
  // Wait for the VPS mini-node handshake to validate so requestRoutes() has
  // someone to talk to.
  for (int i = 0; i < 20; i++) {
    if (rats.validatedPeerIds.isNotEmpty) break;
    await Future.delayed(const Duration(seconds: 1));
  }
  if (rats.validatedPeerIds.isEmpty) {
    throw StateError('no validated peers — the VPS mini-node never handshook');
  }
  final vpsPeer = rats.validatedPeerIds.first;

  final list = await rats.requestRoutes(timeout: const Duration(seconds: 10));
  log('[disc] routes from VPS: ${list.length}');
  Map<String, dynamic>? best;
  int bestSeen = -1;
  for (final r in list) {
    final pid   = r['rats_peer_id']  as String? ?? '';
    final reach = r['reachability']  as String? ?? 'unknown';
    final seen  = r['last_seen_ms']  as int?    ?? 0;
    final pub   = r['public_address'] as String? ?? '';
    log('[disc]   $pid  pub=$pub  reach=$reach  seen=${seen}ms');
    if (pid.isEmpty || pid == rats.ownPeerId) continue;
    // Same routing call LibratsDiscovery makes for every route: direct only
    // when reach=direct AND we have a public address to dial. Anything else
    // routes through the VPS tunnel.
    final canDirect = reach == 'direct' && pub.isNotEmpty;
    if (!canDirect) {
      rats.setRelayVia(pid, vpsPeer);
    } else {
      rats.setRelayVia(pid, null);
    }
    if (seen > bestSeen) { best = r; bestSeen = seen; }
  }
  return best;
}

Future<void> main() async {
  log('[smoke] step 1 — initialize RatsClient (connects to VPS over QUIC)');
  final rats = await RatsClient.initialize();
  log('[smoke]   own peer id : ${rats.ownPeerId}');
  log('[smoke]   public addr : ${rats.publicAddress}');

  log('[smoke] step 2 — discovery (same logic as LibratsDiscovery.refresh)');
  final picked = await runDiscovery(rats);
  if (picked == null) {
    log('[smoke]   no eligible full node in route list — abort');
    rats.dispose();
    exit(2);
  }
  final homePid = picked['rats_peer_id'] as String;
  final reach   = picked['reachability'] as String? ?? 'unknown';
  log('[smoke]   auto-selected = $homePid (reachability=$reach)');

  Future<void> verb(String type, Map<String, dynamic> body) async {
    log('[smoke] -> $type ${jsonEncode(body)}');
    try {
      final reply = await rats.request(homePid, type, body,
          timeout: const Duration(seconds: 12));
      var s = jsonEncode(reply);
      if (s.length > 320) s = '${s.substring(0, 320)}...';
      log('[smoke]    $s');
    } catch (e) {
      log('[smoke]    FAILED: $e');
    }
  }

  log('[smoke] step 3 — RPCs (relay vs direct chosen by setRelayVia)');
  await verb('status', const {});
  await verb('songs.list', const {});
  // upload.open was removed when the home node stopped ingesting audio;
  // the corresponding write path is now fingerprint.submit (registration_smoke.dart).
  await verb('fingerprint.submit', const {
    'fingerprint_hash': '0000000000000000000000000000000000000000000000000000000000000000',
  });

  rats.dispose();
  exit(0);
}
