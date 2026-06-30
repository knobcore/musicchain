// handshake_smoke.dart — tighter test of the QUIC peer-id handshake.
//
// Spins up a RatsClient, calls connect() against either the local home node
// or the VPS mini-node, and prints validatedPeerIds + peerCount every second
// for ~12 s. Useful for isolating whether the handshake stalls on a specific
// hop.

import 'dart:async';
import 'dart:io';

import 'package:bopwire_player/src/services/rats_client.dart';

void main(List<String> args) async {
  final host = args.isNotEmpty ? args[0] : '85.239.238.226';
  final port = args.length > 1 ? int.parse(args[1]) : 443;

  // ignore: avoid_print
  print('[hs] initializing rats client; will connect to $host:$port');
  final rats = await RatsClient.initialize();
  // ignore: avoid_print
  print('[hs] own peer id: ${rats.ownPeerId}');

  final rc = rats.connect(host, port);
  // ignore: avoid_print
  print('[hs] connect rc=$rc');

  for (int i = 0; i < 12; ++i) {
    await Future.delayed(const Duration(seconds: 1));
    final ids = rats.validatedPeerIds;
    // ignore: avoid_print
    print('[hs] t=${i}s peerCount=${rats.peerCount} validated=$ids');
    if (ids.isNotEmpty) break;
  }

  rats.dispose();
  exit(0);
}
