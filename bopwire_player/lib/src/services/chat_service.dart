import 'dart:async';

import 'package:flutter/foundation.dart';

import '../providers/wallet_provider.dart';
import 'chat_publisher.dart';
import 'rats_client.dart';

/// A chat room as advertised by `chat.list_rooms`.
class ChatRoom {
  ChatRoom({
    required this.name,
    required this.topic,
    required this.creator,
    required this.createdMs,
    required this.isPrivate,
  });

  final String name;       // '#'-prefixed, lowercase
  final String topic;
  final String creator;    // 0x-prefixed creator wallet address
  final int    createdMs;
  final bool   isPrivate;

  factory ChatRoom.fromJson(Map j) => ChatRoom(
        name: (j['name'] as String?) ?? '',
        topic: (j['topic'] as String?) ?? '',
        creator: (j['creator'] as String?) ?? '',
        createdMs: (j['created_ms'] as num?)?.toInt() ?? 0,
        isPrivate: (j['private'] as bool?) ?? false,
      );
}

/// A single chat message in a room.
class ChatMessage {
  ChatMessage({
    required this.from,
    required this.fromPubkey,
    required this.tsMs,
    required this.body,
    this.sig = '',
  });

  final String from;        // 0x-prefixed author wallet address
  final String fromPubkey;
  final int    tsMs;
  final String body;
  final String sig;

  factory ChatMessage.fromJson(Map j) => ChatMessage(
        from: (j['from'] as String?) ?? '',
        fromPubkey: (j['from_pubkey'] as String?) ?? '',
        tsMs: (j['ts_ms'] as num?)?.toInt() ?? 0,
        body: (j['body'] as String?) ?? '',
        sig: (j['sig'] as String?) ?? '',
      );

  /// De-dup / ordering key. Two rows from the same author at the same ms with
  /// the same body are treated as identical (covers optimistic echo vs the
  /// pushed copy of our own message).
  String get key => '$from|$tsMs|$body';
}

/// Player-side chat store + transport.
///
/// Claims [RatsClient.onPush] to receive pushed `chat.message` / `chat.kicked`
/// envelopes the mini-node sends with no `req_id`. Exposes per-room message
/// lists; the UI listens via [ChangeNotifier]. Write paths (create / send /
/// moderate) delegate to [ChatPublisher] for canonical signing, then update
/// optimistically.
///
/// Assumed push envelope shapes (see [RatsClient._dispatchReply] →
/// `onPush(peerId, type, body)`):
///   * type "chat.message" body {room, from, from_pubkey, ts_ms, body, sig}
///   * type "chat.kicked"   body {room, target}  (target = kicked wallet hex)
class ChatService extends ChangeNotifier {
  ChatService._();
  static final ChatService instance = ChatService._();

  bool _wired = false;

  /// Per-room message lists, keyed by room name ('#'-prefixed). Each list is
  /// kept ts-sorted ascending (oldest first) for natural chat rendering.
  final Map<String, List<ChatMessage>> _messages = {};

  /// Membership keys so we can drop duplicate pushes / optimistic echoes.
  final Map<String, Set<String>> _seen = {};

  /// Known rooms from the last `chat.list_rooms`.
  List<ChatRoom> _rooms = const [];
  List<ChatRoom> get rooms => _rooms;

  /// The room currently open in the UI (so push handling knows which list is
  /// live). Null when no room view is open.
  String? _openRoom;
  String? get openRoom => _openRoom;

  /// Fires when the local wallet is force-removed from a room ("chat.kicked").
  /// The UI listens to surface a snackbar + pop the room view.
  final StreamController<String> _kicked = StreamController<String>.broadcast();
  Stream<String> get onKicked => _kicked.stream;

  List<ChatMessage> messagesFor(String room) =>
      List.unmodifiable(_messages[room] ?? const <ChatMessage>[]);

  /// Wire up the inbound push hook exactly once. Safe to call repeatedly.
  /// Guarded: RatsClient.instance throws StateError if the rats stack failed
  /// to initialize at startup — we leave _wired false so a later call retries.
  void ensureWired() {
    if (_wired) return;
    try {
      RatsClient.instance.onPush = _onPush;
      _wired = true;
    } catch (_) {/* rats not initialized yet — retry on next ensureWired() */}
  }

  // ---- inbound push --------------------------------------------------------

  void _onPush(String peerId, String type, Map<String, dynamic> body) {
    switch (type) {
      case 'chat.message':
        final room = (body['room'] as String?) ?? '';
        if (room.isEmpty) return;
        _append(room, ChatMessage.fromJson(body), notify: true);
        break;
      case 'chat.kicked':
        final room = (body['room'] as String?) ?? '';
        final target = (body['target'] as String?) ?? '';
        if (room.isEmpty) return;
        // Only react when WE are the kicked wallet. An empty target is treated
        // as "applies to the local user" defensively, but normally the
        // mini-node only pushes chat.kicked to the affected player.
        final me = WalletProvider.active?.info?.address ?? '';
        if (target.isEmpty || _sameAddr(target, me)) {
          if (_openRoom == room) _openRoom = null;
          _kicked.add(room);
        }
        break;
      default:
        // Not a chat push — ignore. (If another subsystem later wants
        // onPush, this is the single claim point to extend.)
        break;
    }
  }

  // ---- read paths ----------------------------------------------------------

  /// Refresh the room directory from the lightest mini-node.
  Future<List<ChatRoom>> listRooms() async {
    final mini = _mini();
    if (mini == null) return _rooms;
    final reply = await RatsClient.instance
        .request(mini, 'chat.list_rooms', const {});
    if (reply is List) {
      _rooms = reply
          .whereType<Map>()
          .map(ChatRoom.fromJson)
          .toList(growable: false)
        ..sort((a, b) => a.name.compareTo(b.name));
      notifyListeners();
    }
    return _rooms;
  }

  /// Back-fill history for [room]. [beforeTsMs] pages older messages (pass the
  /// oldest ts already held); returns the rows fetched this call so the UI can
  /// tell when it has hit the start. Merges into the per-room list.
  Future<List<ChatMessage>> history(
    String room, {
    int? beforeTsMs,
    int limit = 200,
  }) async {
    final mini = _mini();
    if (mini == null) return const [];
    final reply = await RatsClient.instance.request(
      mini,
      'chat.history',
      {
        'room': room,
        'limit': limit,
        if (beforeTsMs != null) 'before_ts_ms': beforeTsMs,
      },
    );
    final out = <ChatMessage>[];
    if (reply is List) {
      for (final e in reply) {
        if (e is Map) out.add(ChatMessage.fromJson(e));
      }
    }
    for (final m in out) {
      _append(room, m, notify: false);
    }
    notifyListeners();
    return out;
  }

  /// Open [room]: marks it current and subscribes for live pushes.
  Future<void> watch(String room) async {
    _openRoom = room;
    final mini = _mini();
    if (mini == null) return;
    try {
      await RatsClient.instance.request(mini, 'chat.watch', {'room': room});
    } catch (_) {/* best effort — history back-fill still works */}
  }

  /// Leave [room]: clears current + unsubscribes.
  Future<void> unwatch(String room) async {
    if (_openRoom == room) _openRoom = null;
    final mini = _mini();
    if (mini == null) return;
    try {
      await RatsClient.instance.request(mini, 'chat.unwatch', {'room': room});
    } catch (_) {/* best effort */}
  }

  // ---- write paths ---------------------------------------------------------

  /// Create a room. [name] must already be normalised by the UI.
  Future<void> createRoom({
    required String name,
    required String topic,
    required bool private,
  }) async {
    final createdMs =
        await ChatPublisher.createRoom(name: name, topic: topic, private: private);
    if (createdMs == null) return;
    final info = WalletProvider.active?.info;
    // Optimistically insert so the new room shows without waiting for the
    // gossip round-trip + next list_rooms.
    if (!_rooms.any((r) => r.name == name)) {
      _rooms = [
        ..._rooms,
        ChatRoom(
          name: name,
          topic: topic,
          creator: info?.address ?? '',
          createdMs: createdMs,
          isPrivate: private,
        ),
      ]..sort((a, b) => a.name.compareTo(b.name));
      notifyListeners();
    }
  }

  /// Send a message to [room], optimistically appending our own copy.
  Future<void> sendMessage(String room, String body) async {
    final trimmed = body.trim();
    if (trimmed.isEmpty) return;
    final info = WalletProvider.active?.info;
    final ts = await ChatPublisher.sendMessage(room, trimmed);
    if (ts == null || info == null) return;
    _append(
      room,
      ChatMessage(
        from: info.address,
        fromPubkey: info.publicKey,
        tsMs: ts,
        body: trimmed,
      ),
      notify: true,
    );
  }

  /// Kick [target] (wallet hex) from [room].
  Future<void> kickUser(String room, String target) =>
      ChatPublisher.moderate(action: 'kick_user', room: room, target: target);

  /// Remove [room] entirely.
  Future<void> removeRoom(String room) async {
    await ChatPublisher.moderate(action: 'remove_room', room: room, target: '');
    _rooms = _rooms.where((r) => r.name != room).toList(growable: false);
    _messages.remove(room);
    _seen.remove(room);
    if (_openRoom == room) _openRoom = null;
    notifyListeners();
  }

  // ---- internals -----------------------------------------------------------

  void _append(String room, ChatMessage m, {required bool notify}) {
    if (m.from.isEmpty || m.tsMs == 0) return;
    final seen = _seen.putIfAbsent(room, () => <String>{});
    if (!seen.add(m.key)) return; // duplicate (echo / re-fetch)
    final list = _messages.putIfAbsent(room, () => <ChatMessage>[]);
    list.add(m);
    list.sort((a, b) => a.tsMs.compareTo(b.tsMs));
    if (notify) notifyListeners();
  }

  String? _mini() =>
      RatsClient.instance.bestMiniNodePeerId ??
      RatsClient.instance.firstMiniNodePeerId;

  bool _sameAddr(String a, String b) =>
      _norm(a) == _norm(b) && _norm(a).isNotEmpty;

  String _norm(String a) {
    var s = a.toLowerCase();
    if (s.startsWith('0x')) s = s.substring(2);
    return s;
  }

  @override
  void dispose() {
    _kicked.close();
    super.dispose();
  }
}
