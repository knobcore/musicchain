// public_addr_smoke.dart — verify that RatsClient._start() populates
// _publicAddress via the new stun.observe verb (no native STUN).

import 'dart:async';
import 'dart:io';

import 'package:bopwire_player/src/services/rats_client.dart';

void log(String msg) {
  // ignore: avoid_print
  print(msg);
}

Future<void> main() async {
  log('=== public_addr_smoke ===');
  final rats = await RatsClient.initialize();
  log('  own peer id           : ${rats.ownPeerId}');
  log('  public addr (initial) : "${rats.publicAddress}"');

  // The new code path fires unawaited(); give it a few seconds.
  for (int i = 0; i < 12; ++i) {
    await Future.delayed(const Duration(seconds: 1));
    if (rats.publicAddress.isNotEmpty) break;
  }
  log('  public addr (after)   : "${rats.publicAddress}"');
  log(rats.publicAddress.isEmpty
      ? '  FAIL: publicAddress was never populated'
      : '  OK: publicAddress populated via stun.observe');

  rats.dispose();
  exit(rats.publicAddress.isEmpty ? 1 : 0);
}
