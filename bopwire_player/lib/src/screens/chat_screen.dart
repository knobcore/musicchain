import 'dart:async';

import 'package:flutter/material.dart';

import '../providers/wallet_provider.dart';
import '../services/chat_service.dart';
import '../services/node_service.dart';
import '../services/rats_client.dart';

/// CHAT tab. Replaces the old Search tab.
///
///   * Room directory (chat.list_rooms) + a "Create room" FAB.
///   * Tap a room → a room view: history back-fill + live append from
///     [ChatService], a composer (chat.send), and paged scroll-back
///     (chat.history before_ts_ms).
///   * Author display resolves on-chain usernames via the NODE rpc
///     `username.lookup`, cached per wallet.
///   * Moderation (kick / remove room) shows only for the room creator or a
///     global moderator (mod.list_moderators).
class ChatScreen extends StatefulWidget {
  const ChatScreen({super.key});

  @override
  State<ChatScreen> createState() => _ChatScreenState();
}

class _ChatScreenState extends State<ChatScreen> {
  final ChatService _chat = ChatService.instance;

  @override
  void initState() {
    super.initState();
    // Claim the inbound push hook + load the moderator set + room directory.
    _chat.ensureWired();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      ChatNames.instance.ensureModeratorsLoaded();
      _refresh();
    });
  }

  Future<void> _refresh() async {
    try {
      await _chat.listRooms();
    } catch (_) {/* offline — show whatever we already have */}
  }

  void _openRoom(ChatRoom room) {
    Navigator.of(context).push(MaterialPageRoute(
      builder: (_) => _RoomView(room: room),
    ));
  }

  Future<void> _showCreateDialog() async {
    final created = await showDialog<bool>(
      context: context,
      builder: (_) => const _CreateRoomDialog(),
    );
    if (created == true) _refresh();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Chat'),
        actions: [
          IconButton(
            icon: const Icon(Icons.refresh),
            tooltip: 'Refresh rooms',
            onPressed: _refresh,
          ),
        ],
      ),
      floatingActionButton: FloatingActionButton.extended(
        onPressed: _showCreateDialog,
        icon: const Icon(Icons.add),
        label: const Text('Create room'),
      ),
      body: AnimatedBuilder(
        animation: _chat,
        builder: (context, _) {
          final rooms = _chat.rooms;
          if (rooms.isEmpty) {
            return RefreshIndicator(
              onRefresh: _refresh,
              child: ListView(
                children: const [
                  SizedBox(height: 120),
                  Center(child: Text('No rooms yet. Create one!')),
                ],
              ),
            );
          }
          return RefreshIndicator(
            onRefresh: _refresh,
            child: ListView.separated(
              itemCount: rooms.length,
              separatorBuilder: (_, __) => const Divider(height: 1),
              itemBuilder: (context, i) {
                final r = rooms[i];
                return ListTile(
                  leading: CircleAvatar(
                    child: Icon(r.isPrivate ? Icons.lock : Icons.tag),
                  ),
                  title: Text(r.name),
                  subtitle: r.topic.isEmpty
                      ? null
                      : Text(r.topic,
                          maxLines: 1, overflow: TextOverflow.ellipsis),
                  trailing: const Icon(Icons.chevron_right),
                  onTap: () => _openRoom(r),
                );
              },
            ),
          );
        },
      ),
    );
  }
}

// ---- Create-room dialog ---------------------------------------------------

class _CreateRoomDialog extends StatefulWidget {
  const _CreateRoomDialog();

  @override
  State<_CreateRoomDialog> createState() => _CreateRoomDialogState();
}

class _CreateRoomDialogState extends State<_CreateRoomDialog> {
  final _nameCtl = TextEditingController();
  final _topicCtl = TextEditingController();
  bool _private = false;
  bool _busy = false;

  @override
  void dispose() {
    _nameCtl.dispose();
    _topicCtl.dispose();
    super.dispose();
  }

  /// Normalise the raw name to the node's rule: auto-prefix '#', lowercase,
  /// sanitise the body to [a-z0-9_-], clamp to 1-32 chars total (incl. '#').
  String _sanitize(String raw) {
    var s = raw.toLowerCase().trim();
    if (s.startsWith('#')) s = s.substring(1);
    s = s.replaceAll(RegExp(r'[^a-z0-9_-]'), '');
    if (s.isEmpty) return '';
    // 32-char cap includes the '#'.
    if (s.length > 31) s = s.substring(0, 31);
    return '#$s';
  }

  Future<void> _submit() async {
    final name = _sanitize(_nameCtl.text);
    if (name.isEmpty || name == '#') {
      ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
        content: Text('Enter a valid name (a-z, 0-9, _ or -).'),
      ));
      return;
    }
    setState(() => _busy = true);
    try {
      await ChatService.instance.createRoom(
        name: name,
        topic: _topicCtl.text.trim(),
        private: _private,
      );
      if (mounted) Navigator.of(context).pop(true);
    } catch (e) {
      if (mounted) {
        setState(() => _busy = false);
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(
          content: Text('Create failed: $e'),
        ));
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    final preview = _sanitize(_nameCtl.text);
    return AlertDialog(
      title: const Text('Create room'),
      content: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          TextField(
            controller: _nameCtl,
            autofocus: true,
            maxLength: 32,
            decoration: InputDecoration(
              labelText: 'Name',
              hintText: 'general',
              prefixText: '#',
              helperText: preview.isEmpty ? null : 'Will be created as $preview',
            ),
            onChanged: (_) => setState(() {}),
          ),
          TextField(
            controller: _topicCtl,
            decoration: const InputDecoration(
              labelText: 'Topic (optional)',
            ),
          ),
          const SizedBox(height: 8),
          SwitchListTile(
            contentPadding: EdgeInsets.zero,
            title: const Text('Private'),
            value: _private,
            onChanged: _busy ? null : (v) => setState(() => _private = v),
          ),
        ],
      ),
      actions: [
        TextButton(
          onPressed: _busy ? null : () => Navigator.of(context).pop(false),
          child: const Text('Cancel'),
        ),
        FilledButton(
          onPressed: _busy ? null : _submit,
          child: _busy
              ? const SizedBox(
                  width: 16, height: 16, child: CircularProgressIndicator(strokeWidth: 2))
              : const Text('Create'),
        ),
      ],
    );
  }
}

// ---- Room view ------------------------------------------------------------

class _RoomView extends StatefulWidget {
  const _RoomView({required this.room});
  final ChatRoom room;

  @override
  State<_RoomView> createState() => _RoomViewState();
}

class _RoomViewState extends State<_RoomView> {
  final ChatService _chat = ChatService.instance;
  final _composer = TextEditingController();
  final _scroll = ScrollController();

  StreamSubscription<String>? _kickSub;
  bool _loadingOlder = false;
  bool _reachedStart = false;
  bool _sending = false;

  String get _room => widget.room.name;

  @override
  void initState() {
    super.initState();
    _chat.watch(_room);
    _kickSub = _chat.onKicked.listen(_onKicked);
    WidgetsBinding.instance.addPostFrameCallback((_) => _backfill());
    _scroll.addListener(_maybeLoadOlder);
  }

  @override
  void dispose() {
    _kickSub?.cancel();
    _chat.unwatch(_room);
    _composer.dispose();
    _scroll.removeListener(_maybeLoadOlder);
    _scroll.dispose();
    super.dispose();
  }

  void _onKicked(String room) {
    if (room != _room || !mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(
      content: Text('You were removed from $room'),
    ));
    Navigator.of(context).maybePop();
  }

  Future<void> _backfill() async {
    try {
      await _chat.history(_room, limit: 200);
    } catch (_) {/* offline */}
    _jumpToBottom();
  }

  void _maybeLoadOlder() {
    // The list is reversed (newest at bottom = scroll offset 0), so older
    // history lives at the *max* extent.
    if (_scroll.position.pixels >=
        _scroll.position.maxScrollExtent - 80) {
      _loadOlder();
    }
  }

  Future<void> _loadOlder() async {
    if (_loadingOlder || _reachedStart) return;
    final current = _chat.messagesFor(_room);
    if (current.isEmpty) return;
    final oldestTs = current.first.tsMs;
    setState(() => _loadingOlder = true);
    try {
      final fetched =
          await _chat.history(_room, beforeTsMs: oldestTs, limit: 200);
      if (fetched.isEmpty) _reachedStart = true;
    } catch (_) {/* offline */} finally {
      if (mounted) setState(() => _loadingOlder = false);
    }
  }

  void _jumpToBottom() {
    if (!_scroll.hasClients) return;
    // reversed list → bottom is offset 0.
    _scroll.jumpTo(0);
  }

  Future<void> _send() async {
    final text = _composer.text.trim();
    if (text.isEmpty) return;
    setState(() => _sending = true);
    _composer.clear();
    try {
      await _chat.sendMessage(_room, text);
      _jumpToBottom();
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(
          content: Text('Send failed: $e'),
        ));
      }
    } finally {
      if (mounted) setState(() => _sending = false);
    }
  }

  /// Local wallet is allowed to moderate this room if it is the creator OR a
  /// global moderator.
  bool get _canModerate {
    final me = WalletProvider.active?.info?.address ?? '';
    if (me.isEmpty) return false;
    if (ChatNames.sameAddr(me, widget.room.creator)) return true;
    return ChatNames.instance.isModerator(me);
  }

  Future<void> _confirmRemoveRoom() async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (_) => AlertDialog(
        title: Text('Remove $_room?'),
        content: const Text('This removes the room for everyone.'),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(context).pop(false),
            child: const Text('Cancel'),
          ),
          FilledButton(
            onPressed: () => Navigator.of(context).pop(true),
            child: const Text('Remove'),
          ),
        ],
      ),
    );
    if (ok != true) return;
    try {
      await _chat.removeRoom(_room);
      if (mounted) Navigator.of(context).maybePop();
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(
          content: Text('Remove failed: $e'),
        ));
      }
    }
  }

  Future<void> _kick(ChatMessage m) async {
    try {
      await _chat.kickUser(_room, m.from);
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
          content: Text('Kick sent'),
        ));
      }
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(
          content: Text('Kick failed: $e'),
        ));
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          mainAxisSize: MainAxisSize.min,
          children: [
            Text(_room),
            if (widget.room.topic.isNotEmpty)
              Text(widget.room.topic,
                  style: Theme.of(context).textTheme.bodySmall,
                  maxLines: 1, overflow: TextOverflow.ellipsis),
          ],
        ),
        actions: [
          if (_canModerate)
            IconButton(
              tooltip: 'Remove room',
              icon: const Icon(Icons.delete_outline),
              onPressed: _confirmRemoveRoom,
            ),
        ],
      ),
      body: Column(
        children: [
          if (_loadingOlder)
            const LinearProgressIndicator(minHeight: 2),
          Expanded(
            child: AnimatedBuilder(
              animation: _chat,
              builder: (context, _) {
                final msgs = _chat.messagesFor(_room);
                if (msgs.isEmpty) {
                  return const Center(child: Text('No messages yet.'));
                }
                // Reversed: index 0 = newest, rendered at the bottom.
                return ListView.builder(
                  controller: _scroll,
                  reverse: true,
                  padding: const EdgeInsets.symmetric(vertical: 8),
                  itemCount: msgs.length,
                  itemBuilder: (context, i) {
                    final m = msgs[msgs.length - 1 - i];
                    return _MessageRow(
                      message: m,
                      showKick: _canModerate &&
                          !ChatNames.sameAddr(
                              m.from,
                              WalletProvider.active?.info?.address ?? ''),
                      onKick: () => _kick(m),
                    );
                  },
                );
              },
            ),
          ),
          const Divider(height: 1),
          _Composer(
            controller: _composer,
            sending: _sending,
            onSend: _send,
          ),
        ],
      ),
    );
  }
}

class _MessageRow extends StatelessWidget {
  const _MessageRow({
    required this.message,
    required this.showKick,
    required this.onKick,
  });

  final ChatMessage message;
  final bool showKick;
  final VoidCallback onKick;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final ts = DateTime.fromMillisecondsSinceEpoch(message.tsMs);
    final time =
        '${ts.hour.toString().padLeft(2, '0')}:${ts.minute.toString().padLeft(2, '0')}';
    return ListTile(
      dense: true,
      leading: CircleAvatar(
        radius: 16,
        child: Text(_initial(message.from)),
      ),
      title: Row(
        children: [
          Flexible(
            child: FutureBuilder<String>(
              future: ChatNames.instance.displayName(message.from),
              initialData: ChatNames.instance.cachedName(message.from),
              builder: (context, snap) => Text(
                snap.data ?? ChatNames.truncate(message.from),
                style: const TextStyle(fontWeight: FontWeight.w600),
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
              ),
            ),
          ),
          const SizedBox(width: 8),
          Text(time, style: theme.textTheme.labelSmall),
        ],
      ),
      subtitle: Text(message.body),
      trailing: showKick
          ? IconButton(
              tooltip: 'Kick user',
              icon: const Icon(Icons.block, size: 18),
              onPressed: onKick,
            )
          : null,
    );
  }

  String _initial(String addr) {
    final s = addr.startsWith('0x') ? addr.substring(2) : addr;
    return s.isEmpty ? '?' : s.substring(0, 1).toUpperCase();
  }
}

class _Composer extends StatelessWidget {
  const _Composer({
    required this.controller,
    required this.sending,
    required this.onSend,
  });

  final TextEditingController controller;
  final bool sending;
  final VoidCallback onSend;

  @override
  Widget build(BuildContext context) {
    return SafeArea(
      top: false,
      child: Padding(
        padding: const EdgeInsets.fromLTRB(12, 6, 6, 6),
        child: Row(
          children: [
            Expanded(
              child: TextField(
                controller: controller,
                minLines: 1,
                maxLines: 4,
                textInputAction: TextInputAction.send,
                decoration: const InputDecoration(
                  hintText: 'Message',
                  border: OutlineInputBorder(),
                  isDense: true,
                ),
                onSubmitted: (_) => onSend(),
              ),
            ),
            const SizedBox(width: 6),
            IconButton.filled(
              icon: sending
                  ? const SizedBox(
                      width: 18, height: 18,
                      child: CircularProgressIndicator(strokeWidth: 2))
                  : const Icon(Icons.send),
              onPressed: sending ? null : onSend,
            ),
          ],
        ),
      ),
    );
  }
}

// ---- Author-name + moderator resolution -----------------------------------

/// Resolves on-chain usernames for wallet addresses (NODE rpc
/// `username.lookup`) and tracks the global-moderator set (NODE rpc
/// `mod.list_moderators`). Both target the home FULL node (homePid via
/// [NodeService.getRatsPeerId]) — these are node verbs, not mini-node ones.
class ChatNames {
  ChatNames._();
  static final ChatNames instance = ChatNames._();

  /// addr(lowercased, no 0x) -> resolved username ('' = looked up, no name).
  final Map<String, String> _names = {};
  final Map<String, Future<String>> _inflight = {};

  /// Normalised moderator addresses (lowercased, no 0x).
  final Set<String> _moderators = {};
  bool _modsLoaded = false;

  static String _norm(String a) {
    var s = a.toLowerCase();
    if (s.startsWith('0x')) s = s.substring(2);
    return s;
  }

  static bool sameAddr(String a, String b) {
    final na = _norm(a), nb = _norm(b);
    return na.isNotEmpty && na == nb;
  }

  static String truncate(String addr) {
    final s = addr.startsWith('0x') ? addr : '0x$addr';
    if (s.length <= 12) return s;
    return '${s.substring(0, 8)}…${s.substring(s.length - 4)}';
  }

  /// Synchronous cache read for FutureBuilder.initialData. Returns the cached
  /// display string (username if non-empty, else truncated address) or null
  /// when this address hasn't been resolved yet.
  String? cachedName(String addr) {
    final key = _norm(addr);
    final n = _names[key];
    if (n == null) return null;
    return n.isNotEmpty ? n : truncate(addr);
  }

  /// Resolve + cache the display name for [addr]. Renders the username when
  /// non-empty, else the truncated 0x address. De-dups concurrent lookups.
  Future<String> displayName(String addr) async {
    final key = _norm(addr);
    final cached = _names[key];
    if (cached != null) return cached.isNotEmpty ? cached : truncate(addr);
    final inflight = _inflight[key];
    if (inflight != null) {
      final n = await inflight;
      return n.isNotEmpty ? n : truncate(addr);
    }
    final fut = _lookup(addr, key);
    _inflight[key] = fut;
    final name = await fut;
    return name.isNotEmpty ? name : truncate(addr);
  }

  Future<String> _lookup(String addr, String key) async {
    var name = '';
    try {
      final homePid =
          await NodeService.getRatsPeerId(waitFor: const Duration(seconds: 4));
      if (homePid.isNotEmpty) {
        final reply = await RatsClient.instance
            .request(homePid, 'username.lookup', {'addr': addr});
        if (reply is Map) name = (reply['username'] as String?) ?? '';
      }
    } catch (_) {/* offline / unknown — fall back to truncated address */}
    _names[key] = name;
    _inflight.remove(key);
    return name;
  }

  bool isModerator(String addr) => _moderators.contains(_norm(addr));

  /// Fetch the active global-moderator set once. Idempotent.
  Future<void> ensureModeratorsLoaded() async {
    if (_modsLoaded) return;
    _modsLoaded = true;
    try {
      final homePid =
          await NodeService.getRatsPeerId(waitFor: const Duration(seconds: 4));
      if (homePid.isEmpty) {
        _modsLoaded = false; // retry next open
        return;
      }
      final reply = await RatsClient.instance
          .request(homePid, 'mod.list_moderators', const {});
      if (reply is Map) {
        final list = (reply['moderators'] as List?) ?? const [];
        _moderators
          ..clear()
          ..addAll(list.whereType<String>().map(_norm));
      }
    } catch (_) {
      _modsLoaded = false; // retry next open
    }
  }
}
