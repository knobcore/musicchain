// mint_smoke.dart — verify the chain's session.start → heartbeat →
// session.complete → wallet.balance pipeline actually mints rewards
// against a live home node. Reproduces the player's flow exactly:
//
//   1. RatsClient.initialize → connect to VPS
//   2. routes.get → pick the home node + set relay
//   3. derive a fresh wallet via mc_wallet_from_mnemonic so the address
//      is EIP-55 0x-prefixed (the new canonical form)
//   4. wallet.balance — report the pre-play balance
//   5. songs.list → grab a content_hash (any song works for the listen
//      check; mint applies whether or not it's actually on chain)
//   6. session.start with our wallet address
//   7. send 19 heartbeats simulating 95 s of playback at 5 s cadence
//      with position monotonically advancing — that's > 30 s legacy
//      threshold AND >= 50 % of any 3-min song
//   8. session.complete — log the full response body
//   9. wallet.balance — confirm it incremented
//
// Run with: dart run tool/mint_smoke.dart

import 'dart:async';
import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';
import 'package:bopwire_player/src/ffi/native_library.dart';
import 'package:bopwire_player/src/ffi/wallet_mnemonic_bindings.dart';
import 'package:bopwire_player/src/services/rats_client.dart';

void log(String msg) {
  // ignore: avoid_print
  print(msg);
}

Future<void> main() async {
  log('=== mint_smoke ===\n');

  await NativeLibrary.initialize();

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
  if (reach == 'direct' && pub.isNotEmpty) {
    rats.setRelayVia(home, null);
  } else {
    rats.setRelayVia(home, vps);
  }
  log('');

  log('step 3 — derive fresh wallet via libwally');
  final mnemonic = WalletMnemonicBindings.bip39Generate12();
  if (mnemonic == null) {
    log('FAIL: bip39_generate_12 returned null');
    rats.dispose();
    exit(4);
  }
  final handle = WalletMnemonicBindings.walletFromMnemonic(mnemonic);
  if (handle == null) {
    log('FAIL: wallet_from_mnemonic returned null');
    rats.dispose();
    exit(5);
  }
  final addrPtr = NativeLibrary.bindings.mc_wallet_get_address(handle);
  final address = addrPtr.cast<Utf8>().toDartString();
  NativeLibrary.bindings.mc_free(addrPtr.cast());
  log('  mnemonic  : ${mnemonic.split(" ").take(3).join(" ")}...');
  log('  address   : $address');
  log('');

  log('step 4 — pre-play balance');
  final balBefore = await rats.request(home, 'wallet.balance',
      {'address': address}, timeout: const Duration(seconds: 8));
  log('  $balBefore\n');

  log('step 5 — songs.list');
  final songsBody = await rats.request(home, 'songs.list', const {},
      timeout: const Duration(seconds: 15));
  final songsList = (songsBody as List?) ?? const [];
  if (songsList.isEmpty) {
    log('FAIL: home library is empty — register a song first');
    rats.dispose();
    exit(6);
  }
  Map<String, dynamic>? song;
  for (final s in songsList) {
    final m = (s as Map?)?.cast<String, dynamic>() ?? const {};
    if ((m['content_hash'] as String? ?? '').isNotEmpty) { song = m; break; }
  }
  if (song == null) {
    log('FAIL: no song has a content_hash');
    rats.dispose();
    exit(7);
  }
  final hash  = song['content_hash'] as String;
  final title = song['title']        as String? ?? '';
  final dur   = (song['duration_ms'] as num?)?.toInt() ?? 0;
  log('  song      : "$title"');
  log('  hash      : ${hash.substring(0, 16)}...');
  log('  duration  : ${dur} ms\n');

  log('step 6 — session.start');
  final startBody = await rats.request(home, 'session.start',
      {'content_hash': hash, 'player_address': address},
      timeout: const Duration(seconds: 8));
  final startMap = (startBody as Map?)?.cast<String, dynamic>() ?? const {};
  final sessionId = startMap['session_id'] as String? ?? '';
  if (sessionId.isEmpty) {
    log('FAIL: session.start returned no session_id: $startBody');
    rats.dispose();
    exit(8);
  }
  log('  session_id: ${sessionId.substring(0, 16)}...\n');

  log('step 7 — heartbeats (95 s simulated, 5 s cadence, 19 beats)');
  for (int i = 0; i < 19; ++i) {
    final pos = (i + 1) * 5000; // 5s, 10s, ..., 95s
    final hbBody = await rats.request(home, 'session.heartbeat',
        {'session_id': sessionId, 'position_ms': pos},
        timeout: const Duration(seconds: 6));
    if (i == 0 || i == 18) {
      log('  beat ${i + 1}: pos=$pos ms -> $hbBody');
    }
    await Future.delayed(const Duration(seconds: 5));
  }
  log('');

  log('step 8 — session.complete');
  final completeBody = await rats.request(home, 'session.complete',
      {'session_id': sessionId},
      timeout: const Duration(seconds: 10));
  log('  $completeBody\n');

  log('step 9 — post-play balance');
  final balAfter = await rats.request(home, 'wallet.balance',
      {'address': address}, timeout: const Duration(seconds: 8));
  log('  $balAfter');

  final beforeAmt = ((balBefore as Map?)?['balance'] as String?) ?? '0.00000000';
  final afterAmt  = ((balAfter  as Map?)?['balance'] as String?) ?? '0.00000000';
  log('');
  log('  before: $beforeAmt');
  log('  after : $afterAmt');
  if (afterAmt == beforeAmt) {
    log('  RESULT: no balance change — mint did not credit player_address');
  } else {
    log('  RESULT: balance INCREASED — mint succeeded');
  }

  rats.dispose();
  exit(0);
}
