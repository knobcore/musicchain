import 'dart:convert';
import 'dart:typed_data';

import '../providers/wallet_provider.dart';
import 'library_scanner.dart';
import 'library_service.dart';
import 'node_service.dart';
import 'rats_client.dart';

/// DB2 — publishes the wallet's library to the off-chain, gossip-replicated
/// LibraryStore on the full node as a wallet-SIGNED delta. The node verifies
/// the signature, applies it (version-gated), then floods it to every other
/// node — so the library clones onto the whole mesh as it changes.
///
/// Canonical signed bytes (MUST match the node's `library_canonical` in
/// rats_api.cpp byte-for-byte or the signature won't verify):
///   "mclib1" || wallet(20) || version(8 LE) || ts(8 LE) ||
///   add_count(4 LE) || add_hashes(32·n) || del_count(4 LE) || del_hashes(32·m)
/// The native wallet signer SHA-256s these bytes and ECDSA-signs the digest;
/// the node verifies the same way (verify_data == sha256 + verify_ecdsa).
class LibraryPublisher {
  static bool _inFlight = false;

  /// Publish the wallet's whole current library as a full delta (add =
  /// every content hash, del = none). No-op without a wallet, an empty
  /// library, or a reachable full node. Safe to call repeatedly — the node's
  /// version gate makes it idempotent.
  static Future<void> publishFull() async {
    if (_inFlight) return;
    _inFlight = true;
    try {
      final wp = WalletProvider.active;
      final info = wp?.info;
      if (wp == null || info == null) return;

      final lib = LibraryService.instance;
      await lib.ensureLoaded();
      final hashes = <String>[
        for (final e in lib.entries)
          if (e.contentHash.isNotEmpty) e.contentHash,
      ];
      if (hashes.isEmpty) return;

      final homePid =
          await NodeService.getRatsPeerId(waitFor: const Duration(seconds: 8));
      if (homePid.isEmpty) return;

      // version == ts (wall-clock ms): monotonic without a stored counter, and
      // roughly consistent across a user's devices (the later edit wins).
      final ts = DateTime.now().millisecondsSinceEpoch;

      final wallet20 = _hexToBytes(info.address, 20);
      if (wallet20 == null) return;
      final add32 = <Uint8List>[];
      for (final h in hashes) {
        final b = _hexToBytes(h, 32);
        if (b == null) return; // a malformed hash must not poison the signature
        add32.add(b);
      }

      final canon = BytesBuilder();
      canon.add(ascii.encode('mclib1')); // 6-byte domain tag
      canon.add(wallet20);               // 20
      canon.add(_u64le(ts));             // version (8 LE)
      canon.add(_u64le(ts));             // ts      (8 LE)
      canon.add(_u32le(add32.length));   // add_count (4 LE)
      for (final b in add32) {
        canon.add(b);                    // 32 each
      }
      canon.add(_u32le(0));              // del_count = 0 (full publish)

      final sig = wp.sign(Uint8List.fromList(canon.toBytes()));

      final reply = await RatsClient.instance.request(
        homePid,
        'library.delta',
        {
          'wallet': info.address,
          'pubkey': info.publicKey,
          'version': ts,
          'ts': ts,
          'add': hashes,
          'del': const <String>[],
          'sig': sig,
        },
        timeout: const Duration(seconds: 10),
      );

      final applied = (reply is Map) && (reply['applied'] == true);
      // The node replies with `unknown[]`: the published content hashes that
      // aren't registered on chain yet (fresh chain, or one wiped between
      // sessions). Re-fire fingerprint.submit for each so the songs actually
      // land in the mempool and get minted — library.delta alone only records
      // off-chain library membership, not the chain. This is the resubmit the
      // old swarm.hello reply path used to drive; it now rides on DB2.
      final unknown = (reply is Map)
          ? ((reply['unknown'] as List?)?.cast<String>() ?? const <String>[])
          : const <String>[];
      // ignore: avoid_print
      print('[db2] library.delta v$ts add=${hashes.length} applied=$applied'
            ' unknown=${unknown.length}');
      if (unknown.isNotEmpty) {
        // Released the in-flight guard in finally below; resubmit is awaited so
        // its _processFile work completes before publishFull returns (matches
        // the old scanOnce ordering under the scanner's _running guard).
        await LibraryScanner.instance.resubmitUnknown(unknown);
      }
    } catch (e) {
      // ignore: avoid_print
      print('[db2] publishFull failed: $e');
    } finally {
      _inFlight = false;
    }
  }

  static Uint8List _u64le(int x) {
    final b = Uint8List(8);
    for (int i = 0; i < 8; i++) {
      b[i] = (x >> (8 * i)) & 0xFF;
    }
    return b;
  }

  static Uint8List _u32le(int x) {
    final b = Uint8List(4);
    for (int i = 0; i < 4; i++) {
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
