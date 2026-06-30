// stun_observe_smoke.dart — verify that the new stun.observe verb on the
// mini-node hands the caller back its own externally-visible address. Same
// QUIC stream the rest of the protocol uses, no extra ports.

import 'dart:async';
import 'dart:io';

import 'package:bopwire_player/src/services/rats_client.dart';

void log(String msg) {
  // ignore: avoid_print
  print(msg);
}

Future<void> main() async {
  log('=== stun.observe smoke (VPS mini-node @ UDP/443) ===');
  final rats = await RatsClient.initialize();
  log('  own peer id : ${rats.ownPeerId}');

  // Wait for VPS handshake.
  for (int i = 0; i < 15; i++) {
    if (rats.validatedPeerIds.isNotEmpty) break;
    await Future.delayed(const Duration(seconds: 1));
  }
  if (rats.validatedPeerIds.isEmpty) {
    log('  FAILED: VPS handshake never completed');
    rats.dispose();
    exit(2);
  }
  final vps = rats.validatedPeerIds.first;
  log('  vps peer id : $vps');

  log('-> stun.observe');
  try {
    final reply = await rats.request(vps, 'stun.observe', const {},
        timeout: const Duration(seconds: 6));
    final m = (reply as Map?) ?? const {};
    final addr = m['observed_address'] as String? ?? '';
    log('   observed_address = "$addr"');
    if (addr.isEmpty) {
      log('   FAILED: empty observed_address');
      rats.dispose();
      exit(3);
    }
    log('   OK — we are visible at $addr');
  } catch (e) {
    log('   FAILED: $e');
    rats.dispose();
    exit(4);
  }

  rats.dispose();
  exit(0);
}
