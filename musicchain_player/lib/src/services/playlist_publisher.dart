import 'dart:convert';
import 'dart:typed_data';

import '../providers/wallet_provider.dart';
import 'node_service.dart';
import 'playlist_service.dart';
import 'rats_client.dart';

/// Publishes a playlist as a wallet-SIGNED `playlist.set` record (or a
/// tombstone when deleted); the node verifies the signature, applies it
/// version-gated, and floods it to every node. The canonical signed bytes MUST
/// match the node's `playlist_canonical` ("mcpls1" ...) byte-for-byte.
class PlaylistPublisher {
  static Future<void> publish(Playlist pl, {bool deleted = false}) async {
    try {
      final wp = WalletProvider.active;
      final info = wp?.info;
      if (wp == null || info == null) return;
      final homePid =
          await NodeService.getRatsPeerId(waitFor: const Duration(seconds: 8));
      if (homePid.isEmpty) return;

      // Use the record's STORED version (PlaylistService bumps it on every
      // edit before publishing) so a re-publish on reconnect is idempotent:
      // the node version-gates an unchanged record out instead of re-flooding.
      final version =
          pl.version != 0 ? pl.version : DateTime.now().millisecondsSinceEpoch;
      final ts = version;

      final wallet20 = _hexToBytes(info.address, 20);
      final pid16 = _hexToBytes(pl.id, 16);
      if (wallet20 == null || pid16 == null) return;
      final songs = deleted ? const <String>[] : pl.songs;
      final songs32 = <Uint8List>[];
      for (final h in songs) {
        final b = _hexToBytes(h, 32);
        if (b == null) return;
        songs32.add(b);
      }
      final nameBytes = deleted ? const <int>[] : utf8.encode(pl.name);

      // "mcpls1" || wallet(20) || playlist_id(16) || version(8 LE) ||
      // ts(8 LE) || deleted(1) || name_len(2 LE) || name || count(4 LE) ||
      // song_hashes(32·n)
      final canon = BytesBuilder();
      canon.add(ascii.encode('mcpls1'));
      canon.add(wallet20);
      canon.add(pid16);
      canon.add(_u64le(version));
      canon.add(_u64le(ts));
      canon.add(Uint8List.fromList([deleted ? 1 : 0]));
      canon.add(_u16le(nameBytes.length));
      canon.add(Uint8List.fromList(nameBytes));
      canon.add(_u32le(songs32.length));
      for (final s in songs32) {
        canon.add(s);
      }

      final sig = wp.sign(Uint8List.fromList(canon.toBytes()));

      final reply = await RatsClient.instance.request(
        homePid,
        'playlist.set',
        {
          'wallet': info.address,
          'pubkey': info.publicKey,
          'playlist_id': pl.id,
          'version': version,
          'ts': ts,
          'deleted': deleted,
          'name': deleted ? '' : pl.name,
          'songs': deleted ? const <String>[] : pl.songs,
          'sig': sig,
        },
        timeout: const Duration(seconds: 10),
      );
      final applied = (reply is Map) && (reply['applied'] == true);
      // ignore: avoid_print
      print('[db2] playlist.set ${pl.id.substring(0, 8)} v$version '
          'songs=${songs.length} del=$deleted applied=$applied');
    } catch (e) {
      // ignore: avoid_print
      print('[db2] playlist publish failed: $e');
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

  static Uint8List _u16le(int x) {
    final b = Uint8List(2);
    for (int i = 0; i < 2; i++) {
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
