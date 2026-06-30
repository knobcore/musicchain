import 'dart:async';
import 'dart:convert';
import 'dart:math' as math;
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
  static Timer? _heartbeat;
  static final math.Random _rng = math.Random();

  /// Start the presence heartbeat: a SIGNED presence.hello every ~30 s so the
  /// wallet<->peer binding stays fresh and re-establishes after the node
  /// restarts or a relayed download blips our connection. We sign every beat
  /// (not a sig-less keepalive) because the node trusts ONLY the signature for
  /// discoverability -- a lighter ping would be forgeable on the public
  /// {wallet, peer_id} tuple. The ECDSA cost is trivial at this scale. No-op
  /// without a wallet / own peer-id / reachable node, so it's safe to start at
  /// boot. Idempotent: a second call is ignored.
  static void startHeartbeat() {
    if (_heartbeat != null) return;
    _scheduleNextBeat();
  }

  static void _scheduleNextBeat() {
    // Base 30 s with +/-8 s jitter so a fleet of players doesn't phase-align
    // into synchronized presence spikes at the node. Well under the node's
    // kPresenceTtlMs (180 s), so several missed beats (e.g. a download
    // saturating the relay) don't drop us out of other players' Discover.
    final ms = 30000 + _rng.nextInt(16001) - 8000; // 22-38 s
    _heartbeat = Timer(Duration(milliseconds: ms), () async {
      try {
        await announce();
      } catch (_) {
        // best effort -- next beat retries
      }
      if (_heartbeat != null) _scheduleNextBeat();
    });
  }

  /// Cancel the heartbeat (used on full sign-out / teardown).
  static void stopHeartbeat() {
    _heartbeat?.cancel();
    _heartbeat = null;
  }

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
