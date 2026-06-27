import 'dart:convert';
import 'dart:typed_data';

import '../providers/wallet_provider.dart';
import 'node_service.dart';
import 'rats_client.dart';

/// Presence — declares the player's wallet<->live-peer binding on connect.
///
/// Replaces the old `swarm.hello` library re-announce (DB2 / library.delta now
/// carries the durable library). This sends a small, wallet-SIGNED
/// `presence.hello` so the full node knows which live librats peer-id currently
/// belongs to which wallet — without re-listing the whole library every
/// reconnect.
///
/// Canonical signed bytes (MUST match the node's presence verifier byte-for-byte
/// or the signature won't verify):
///   "mcprs1" || wallet(20) || ts(8 LE) || peer_id(raw UTF-8, NO length prefix)
/// The native wallet signer SHA-256s these bytes and ECDSA-signs the digest; the
/// node rebuilds + verifies the same way.
class PresencePublisher {
  /// Announce the active wallet's binding to its current librats peer-id. No-op
  /// without a wallet, a reachable home node, or our own peer-id yet. Wrapped in
  /// try/catch — best effort; the next reconnect retries.
  static Future<void> announce() async {
    try {
      final wp = WalletProvider.active;
      final info = wp?.info;
      if (wp == null || info == null) return;

      final homePid =
          await NodeService.getRatsPeerId(waitFor: const Duration(seconds: 8));
      if (homePid.isEmpty) return;

      // Our OWN librats peer-id — the SAME source swarm.hello put in `peer_id`.
      final peerId = RatsClient.instance.ownPeerId;
      if (peerId.isEmpty) return;

      final wallet20 = _hexToBytes(info.address, 20);
      if (wallet20 == null) return;

      final ts = DateTime.now().millisecondsSinceEpoch;

      // canon = "mcprs1" || wallet(20) || ts(8 LE) || peer_id(raw UTF-8)
      final canon = BytesBuilder();
      canon.add(ascii.encode('mcprs1')); // 6-byte domain tag
      canon.add(wallet20);               // 20 raw wallet bytes
      canon.add(_u64le(ts));             // ts (8 LE)
      canon.add(utf8.encode(peerId));    // peer_id raw UTF-8, NO length prefix

      final sig = wp.sign(Uint8List.fromList(canon.toBytes()));

      final reply = await RatsClient.instance.request(
        homePid,
        'presence.hello',
        {
          'peer_id': peerId,
          'wallet': info.address,
          'pubkey': info.publicKey,
          'ts': ts,
          'sig': sig,
        },
        timeout: const Duration(seconds: 10),
      );

      final bound = (reply is Map) && (reply['bound'] == true);
      // ignore: avoid_print
      print('[db2] presence.hello ts=$ts peer=$peerId bound=$bound');
    } catch (e) {
      // ignore: avoid_print
      print('[db2] presence.hello failed: $e');
    }
  }

  static Uint8List _u64le(int x) {
    final b = Uint8List(8);
    for (int i = 0; i < 8; i++) {
      b[i] = (x >> (8 * i)) & 0xFF;
    }
    return b;
  }

  static Uint8List? _hexToBytes(String hexIn, int n) {
    var hex = hexIn;
    if (hex.startsWith('0x') || hex.startsWith('0X')) hex = hex.substring(2);
    if (hex.length != n * 2) return null;
    final out = Uint8List(n);
    for (int i = 0; i < n; i++) {
      final v = int.tryParse(hex.substring(2 * i, 2 * i + 2), radix: 16);
      if (v == null) return null;
      out[i] = v;
    }
    return out;
  }
}
