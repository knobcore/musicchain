// library_smoke.dart — minimal dart-run replay of the player's library
// load. Skips Flutter-bound NodeService/NodeClient and exercises the bare
// network path: RatsClient.initialize → routes.get → setRelayVia →
// rats.request(homePid, 'songs.list').

import 'dart:async';
import 'dart:io';

import 'package:bopwire_player/src/services/rats_client.dart';

void log(String msg) { /* ignore: avoid_print */ print(msg); }

Future<Map<String, dynamic>?> runDiscovery(RatsClient rats) async {
  for (int i = 0; i < 20; i++) {
    if (rats.validatedPeerIds.isNotEmpty) break;
    await Future.delayed(const Duration(seconds: 1));
  }
  if (rats.validatedPeerIds.isEmpty) {
    throw StateError('VPS handshake never completed');
  }
  final vpsPeer = rats.validatedPeerIds.first;
  final list = await rats.requestRoutes(timeout: const Duration(seconds: 10));
  log('[disc] ${list.length} routes from VPS');

  Map<String, dynamic>? pick;
  int best = -1;
  for (final r in list) {
    final pid   = r['rats_peer_id']   as String? ?? '';
    final reach = r['reachability']   as String? ?? 'unknown';
    final pub   = r['public_address'] as String? ?? '';
    final seen  = r['last_seen_ms']   as int?    ?? 0;
    final url   = r['api_url']        as String? ?? '';
    log('[disc]   peer=$pid  reach=$reach  pub=$pub  url=$url');
    if (pid.isEmpty || pid == rats.ownPeerId) continue;
    final canDirect = reach == 'direct' && pub.isNotEmpty;
    rats.setRelayVia(pid, canDirect ? null : vpsPeer);
    if (seen > best) { best = seen; pick = r; }
  }
  return pick;
}

Future<void> main() async {
  log('=== library_smoke ===');

  log('step 1 — RatsClient.initialize');
  final rats = await RatsClient.initialize();
  log('  own peer id : ${rats.ownPeerId}');
  for (int i = 0; i < 10 && rats.publicAddress.isEmpty; ++i) {
    await Future.delayed(const Duration(seconds: 1));
  }
  log('  public addr : "${rats.publicAddress}"');

  log('step 2 — discovery');
  final pick = await runDiscovery(rats);
  if (pick == null) {
    log('  FAIL: no full nodes in routes');
    rats.dispose(); exit(2);
  }
  final homePid = pick['rats_peer_id'] as String;
  final url     = pick['api_url']      as String? ?? '';
  log('  auto-selected pid = $homePid');
  log('  auto-selected url = "$url"');
  log('  provider would: ${url.isEmpty
                            ? "THROW (current) / OK rats-path (proposed fix)"
                            : "OK"}');

  log('step 3 — songs.list via the rats path');
  try {
    final reply = await rats.request(homePid, 'songs.list', const {},
        timeout: const Duration(seconds: 15));
    final list = (reply as List?) ?? const [];
    log('  OK: ${list.length} songs');
    for (final e in list.take(5)) {
      final m = (e as Map?) ?? const {};
      log('    - ${m['title']}  (${m['artist']})  album="${m['album']}"');
    }
    if (list.length > 5) log('    + ${list.length - 5} more');
  } catch (e) {
    final s = e.toString();
    log('  FAIL: ${s.length > 240 ? s.substring(0, 240) : s}');
    rats.dispose(); exit(3);
  }

  rats.dispose(); exit(0);
}
