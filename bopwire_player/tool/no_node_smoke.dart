// no_node_smoke.dart — reproduces the "No node discovered yet" path the
// providers raise when LibratsDiscovery hasn't published a rats peer id
// for them yet. Run with `dart run tool/no_node_smoke.dart` to verify the
// startup race between providers and discovery is closed.
//
// Path under test:
//   1. RatsClient.initialize — opens the TCP link to the VPS rendezvous.
//   2. Run the same discovery body LibratsDiscovery.refresh() runs (mirrors
//      it without dragging in Flutter binding init) and publish the picked
//      peer via NodeService.updateAutoNode.
//   3. Read it back via NodeService.getRatsPeerId() and confirm a provider's
//      `_getClient()` would no longer raise "No node discovered yet."

import 'dart:async';
import 'dart:io';

import 'package:bopwire_player/src/services/node_service.dart';
import 'package:bopwire_player/src/services/rats_client.dart';

void log(String msg) {
  // ignore: avoid_print
  print(msg);
}

Future<String> runDiscoveryAndPublish(RatsClient rats) async {
  for (int i = 0; i < 20; i++) {
    if (rats.validatedPeerIds.isNotEmpty) break;
    await Future.delayed(const Duration(seconds: 1));
  }
  if (rats.validatedPeerIds.isEmpty) {
    throw StateError('VPS handshake never completed');
  }
  final vpsPeer = rats.validatedPeerIds.first;

  final list = await rats.requestRoutes(timeout: const Duration(seconds: 10));
  log('[disc] received ${list.length} routes from VPS');

  Map<String, dynamic>? pick;
  int newest = -1;
  for (final r in list) {
    final pid   = r['rats_peer_id']    as String? ?? '';
    final pub   = r['public_address']  as String? ?? '';
    final reach = r['reachability']    as String? ?? 'unknown';
    final seen  = r['last_seen_ms']    as int?    ?? 0;
    log('[disc]   peer=$pid  reach=$reach  pub=$pub');
    if (pid.isEmpty || pid == rats.ownPeerId) continue;
    final canDirect = reach == 'direct' && pub.isNotEmpty;
    if (!canDirect) {
      rats.setRelayVia(pid, vpsPeer);
    } else {
      rats.setRelayVia(pid, null);
    }
    if (seen > newest) { newest = seen; pick = r; }
  }

  final ratsPeerId = (pick?['rats_peer_id'] as String?) ?? '';
  await NodeService.updateAutoNode(ratsPeerId: ratsPeerId);
  return ratsPeerId;
}

Future<void> main() async {
  log('=== no_node_smoke: provider startup race ===\n');

  log('step 1 — RatsClient.initialize');
  final rats = await RatsClient.initialize();
  log('  own peer id : ${rats.ownPeerId}');
  log('  public addr : ${rats.publicAddress}\n');

  log('step 2 — run LibratsDiscovery-equivalent + write NodeService');
  final picked = await runDiscoveryAndPublish(rats);
  log('  picked rats peer id = "$picked"\n');

  log('step 3 — read back via NodeService (what providers see)');
  final readPid = await NodeService.getRatsPeerId(
      waitFor: const Duration(seconds: 2));
  log('  NodeService.getRatsPeerId() -> "$readPid"\n');

  if (readPid.isEmpty) {
    log('FAIL: provider _getClient() would still throw "No node discovered yet."');
    rats.dispose();
    exit(1);
  }
  log('OK: provider _getClient() would resolve and issue rats RPCs to "$readPid".');

  rats.dispose();
  exit(0);
}
