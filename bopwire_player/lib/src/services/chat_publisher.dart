import 'dart:convert';
import 'dart:typed_data';

import '../providers/wallet_provider.dart';
import 'rats_client.dart';

/// Publishes wallet-SIGNED chat actions (messages, room creates, moderation)
/// to a mini-node. The canonical signed bytes MUST match the node/mini-node
/// verifier byte-for-byte or the signature won't verify.
///
/// Wire shape mirrors [PlaylistPublisher] / [PresencePublisher]:
///   * a 6-byte ASCII domain tag,
///   * raw fields (20-byte wallet, utf8 strings),
///   * 0x1F (unit separator, byte 31) between variable-length fields,
///   * u64 little-endian timestamps.
///
/// The native wallet signer (`wp.sign`) SHA-256s the bytes then ECDSA-signs
/// the digest, so we pass the RAW canonical bytes here (NOT a pre-hash). The
/// inline `*_pubkey` field carries `info.publicKey` (66-hex compressed) so the
/// verifier can recover + address-check the signer.
///
/// Domain tags:
///   "mccht1" — chat message
///   "mccrm1" — room create
///   "mccmd1" — moderation action
class ChatPublisher {
  /// 0x1F unit separator between variable-length canonical fields.
  static const int _kUS = 0x1F;

  /// Sign + send a chat message into [room].
  ///
  /// canon = ascii("mccht1") + room(utf8) + [0x1F] + from20 + u64le(ts_ms)
  ///         + body(utf8)
  ///
  /// Returns the `ts_ms` used (so the caller can optimistically render the
  /// same row), or null if there's no wallet / no mini-node / a bad address.
  static Future<int?> sendMessage(String room, String body) async {
    final wp = WalletProvider.active;
    final info = wp?.info;
    if (wp == null || info == null) return null;
    final from20 = _hexToBytes(info.address, 20);
    if (from20 == null) return null;

    final mini = RatsClient.instance.bestMiniNodePeerId
        ?? RatsClient.instance.firstMiniNodePeerId;
    if (mini == null) return null;

    final ts = DateTime.now().millisecondsSinceEpoch;

    final canon = BytesBuilder();
    canon.add(ascii.encode('mccht1'));   // 6-byte domain tag
    canon.add(utf8.encode(room));        // room (utf8)
    canon.addByte(_kUS);                 // 0x1F
    canon.add(from20);                   // from (20 raw bytes)
    canon.add(_u64le(ts));               // ts_ms (8 LE)
    canon.add(utf8.encode(body));        // body (utf8)

    final sig = wp.sign(Uint8List.fromList(canon.toBytes()));

    await RatsClient.instance.request(
      mini,
      'chat.send',
      {
        'room': room,
        'from': info.address,
        'from_pubkey': info.publicKey,
        'ts_ms': ts,
        'body': body,
        'sig': sig,
      },
      timeout: const Duration(seconds: 10),
    );
    return ts;
  }

  /// Sign + send a room-create announce.
  ///
  /// canon = ascii("mccrm1") + name(utf8) + [0x1F] + creator20
  ///         + u64le(created_ms) + (private ? [0x01] : [0x00])
  ///
  /// [name] is expected already normalised by the caller (lowercased,
  /// '#'-prefixed, body sanitised to [a-z0-9_-], 1-32 chars). Returns the
  /// `created_ms` used, or null on failure.
  static Future<int?> createRoom({
    required String name,
    required String topic,
    required bool private,
  }) async {
    final wp = WalletProvider.active;
    final info = wp?.info;
    if (wp == null || info == null) return null;
    final creator20 = _hexToBytes(info.address, 20);
    if (creator20 == null) return null;

    final mini = RatsClient.instance.bestMiniNodePeerId
        ?? RatsClient.instance.firstMiniNodePeerId;
    if (mini == null) return null;

    final createdMs = DateTime.now().millisecondsSinceEpoch;

    final canon = BytesBuilder();
    canon.add(ascii.encode('mccrm1'));    // 6-byte domain tag
    canon.add(utf8.encode(name));         // name (utf8)
    canon.addByte(_kUS);                  // 0x1F
    canon.add(creator20);                 // creator (20 raw bytes)
    canon.add(_u64le(createdMs));         // created_ms (8 LE)
    canon.addByte(private ? 0x01 : 0x00); // private flag

    final sig = wp.sign(Uint8List.fromList(canon.toBytes()));

    await RatsClient.instance.request(
      mini,
      'chat.create',
      {
        'name': name,
        'topic': topic,
        'creator': info.address,
        'creator_pubkey': info.publicKey,
        'created_ms': createdMs,
        'private': private,
        'sig': sig,
      },
      timeout: const Duration(seconds: 10),
    );
    return createdMs;
  }

  /// Sign + send a moderation action.
  ///
  /// canon = ascii("mccmd1") + action(utf8) + [0x1F] + room(utf8) + [0x1F]
  ///         + target(utf8) + [0x1F] + by20 + u64le(ts_ms)
  ///
  /// [action] in {"kick_user","remove_room"}. [target] is the kicked wallet
  /// hex for "kick_user", or "" for "remove_room". Returns the `ts_ms` used,
  /// or null on failure.
  static Future<int?> moderate({
    required String action,
    required String room,
    required String target,
  }) async {
    final wp = WalletProvider.active;
    final info = wp?.info;
    if (wp == null || info == null) return null;
    final by20 = _hexToBytes(info.address, 20);
    if (by20 == null) return null;

    final mini = RatsClient.instance.bestMiniNodePeerId
        ?? RatsClient.instance.firstMiniNodePeerId;
    if (mini == null) return null;

    final ts = DateTime.now().millisecondsSinceEpoch;

    final canon = BytesBuilder();
    canon.add(ascii.encode('mccmd1'));   // 6-byte domain tag
    canon.add(utf8.encode(action));      // action (utf8)
    canon.addByte(_kUS);                 // 0x1F
    canon.add(utf8.encode(room));        // room (utf8)
    canon.addByte(_kUS);                 // 0x1F
    canon.add(utf8.encode(target));      // target (utf8) — "" for remove_room
    canon.addByte(_kUS);                 // 0x1F
    canon.add(by20);                     // by (20 raw bytes)
    canon.add(_u64le(ts));               // ts_ms (8 LE)

    final sig = wp.sign(Uint8List.fromList(canon.toBytes()));

    await RatsClient.instance.request(
      mini,
      'chat.moderate',
      {
        'action': action,
        'room': room,
        'target': target,
        'by': info.address,
        'by_pubkey': info.publicKey,
        'ts_ms': ts,
        'sig': sig,
      },
      timeout: const Duration(seconds: 10),
    );
    return ts;
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
