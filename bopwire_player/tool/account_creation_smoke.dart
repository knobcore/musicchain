// account_creation_smoke.dart — reproduce the "stuck on Create account"
// path without any Flutter UI. Generates a fresh mnemonic, calls every
// step the wallet first-launch screen calls (createWalletFromMnemonic →
// _tryRegisterUsername), and times each one so the place that hangs
// surfaces in the log.

import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:bopwire_player/src/ffi/native_library.dart';
import 'package:bopwire_player/src/services/rats_client.dart';
import 'package:bopwire_player/src/services/wallet_service.dart';

void log(String msg) {
  final ts = DateTime.now().toIso8601String().substring(11, 23);
  // ignore: avoid_print
  print('[$ts] $msg');
}

Future<T> timed<T>(String label, Future<T> Function() fn) async {
  final t0 = DateTime.now();
  try {
    final v = await fn();
    final dt = DateTime.now().difference(t0).inMilliseconds;
    log('  $label OK in ${dt}ms');
    return v;
  } catch (e) {
    final dt = DateTime.now().difference(t0).inMilliseconds;
    log('  $label FAILED after ${dt}ms: $e');
    rethrow;
  }
}

Uint8List hexToBytes(String hex) {
  String s = hex;
  if (s.startsWith('0x') || s.startsWith('0X')) s = s.substring(2);
  if (s.length % 2 != 0) {
    throw FormatException('odd-length hex: ${s.length}');
  }
  final out = Uint8List(s.length ~/ 2);
  for (int i = 0; i < out.length; ++i) {
    out[i] = int.parse(s.substring(i * 2, i * 2 + 2), radix: 16);
  }
  return out;
}

Future<void> main() async {
  log('=== account_creation_smoke ===\n');

  log('step 0 — initialize NativeLibrary');
  await timed('NativeLibrary.initialize', () => NativeLibrary.initialize());

  log('step 1 — RatsClient.initialize (handshake to VPS)');
  final rats = await timed('RatsClient.initialize',
      () => RatsClient.initialize());

  log('step 2 — wait for a mini-node peer');
  String? mini;
  for (int i = 0; i < 30; ++i) {
    mini = rats.firstMiniNodePeerId;
    if (mini != null) break;
    await Future.delayed(const Duration(seconds: 1));
  }
  if (mini == null) {
    log('FAIL: no mini-node identified within 30s');
    rats.dispose();
    exit(2);
  }
  log('  mini-node: ${mini.substring(0, 12)}…');

  log('step 3 — routes.get to find the home node');
  final routes = await timed('rats.requestRoutes',
      () => rats.requestRoutes(timeout: const Duration(seconds: 6)));
  log('  got ${routes.length} routes');
  String? home;
  for (final r in routes) {
    final pid = r['rats_peer_id'] as String? ?? '';
    if (pid.isNotEmpty && pid != rats.ownPeerId) { home = pid; break; }
  }
  if (home == null) {
    log('FAIL: no home node in routes');
    rats.dispose();
    exit(3);
  }
  log('  home: ${home.substring(0, 12)}…\n');

  log('step 4 — wallet service: generate fresh mnemonic');
  final ws = WalletService();
  final mnemonic = await timed('ws.generateMnemonic',
      () async => ws.generateMnemonic());
  if (mnemonic == null || mnemonic.isEmpty) {
    log('FAIL: generateMnemonic returned null/empty');
    rats.dispose();
    exit(4);
  }
  log('  mnemonic: "${mnemonic.split(' ').take(2).join(' ')} …"');

  log('step 5 — createWalletFromMnemonic (mnemonic == password)');
  // Use a unique username per run so collisions don't masquerade as
  // hangs.
  final ts       = DateTime.now().millisecondsSinceEpoch;
  final username = 'smoke_$ts';
  final info = await timed('ws.createWalletFromMnemonic',
      () => ws.createWalletFromMnemonic(
            mnemonic: mnemonic,
            password: mnemonic,
            username: username,
          ));
  log('  addr     : ${info.address}');
  log('  pubkey   : ${info.publicKey.substring(0, 20)}…');
  log('  username : $username\n');

  log('step 6 — wallet.nonce on home');
  final nonceReply = await timed('wallet.nonce',
      () => rats.request(home!, 'wallet.nonce',
          {'address': info.address},
          timeout: const Duration(seconds: 6)));
  final nonce = ((nonceReply as Map?) ?? const {})['nonce'] as int? ?? 0;
  log('  nonce: $nonce\n');

  log('step 7 — build UsernameTx sign_message preimage + sign');
  const int chainId = 19779;
  final nameBytes = utf8.encode(username);
  final addrBytes = hexToBytes(info.address);
  final pkBytes   = hexToBytes(info.publicKey);
  log('  name_bytes=${nameBytes.length} addr_bytes=${addrBytes.length} '
      'pk_bytes=${pkBytes.length}');
  if (addrBytes.length != 20 || pkBytes.length != 33) {
    log('FAIL: bad addr/pubkey length');
    rats.dispose();
    exit(7);
  }
  final bb = BytesBuilder(copy: false);
  for (int s = 0; s < 4; ++s) bb.addByte((chainId >> (8 * s)) & 0xff);
  bb.addByte(nameBytes.length);
  bb.add(nameBytes);
  bb.add(addrBytes);
  bb.add(pkBytes);
  for (int s = 0; s < 8; ++s) bb.addByte((nonce >> (8 * s)) & 0xff);
  final preimage = bb.toBytes();
  log('  preimage_bytes: ${preimage.length}');
  final sigHex = await timed('ws.sign',
      () async => ws.sign(preimage));
  log('  sig: ${sigHex.substring(0, 20)}…\n');

  log('step 8 — submit username.register');
  final result = await timed('username.register',
      () => rats.request(home!, 'username.register', {
            'name':          username,
            'owner_address': info.address,
            'owner_pubkey':  info.publicKey,
            'nonce':         nonce,
            'signature':     sigHex,
          }, timeout: const Duration(seconds: 6)));
  log('  reply: $result\n');

  log('step 9 — quick sanity poll for the username to land in mempool/chain');
  // Just timing how long the producer takes; not gating success on this
  // because the smoke's job is to flush hangs, not to wait on consensus.
  for (int i = 0; i < 5; ++i) {
    await Future.delayed(const Duration(seconds: 2));
    final nReply = await rats.request(home, 'wallet.nonce',
        {'address': info.address},
        timeout: const Duration(seconds: 4));
    final n2 = ((nReply as Map?) ?? const {})['nonce'] as int? ?? 0;
    log('  poll[$i] nonce=$n2');
    if (n2 > nonce) {
      log('  → username tx confirmed (nonce advanced)');
      break;
    }
  }

  log('\nOK — all steps completed without hanging.');
  rats.dispose();
  exit(0);
}
