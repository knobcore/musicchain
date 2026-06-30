import 'dart:async';
import 'dart:convert';
import 'dart:math';

import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'playlist_publisher.dart';

/// A user-owned, ordered saved playlist. `id` is a 16-byte id (32-hex) — the
/// key the DB2 store + gossip use. Songs are content-hash hex strings.
class Playlist {
  final String id;
  String name;
  List<String> songs;
  int version;

  Playlist({
    required this.id,
    required this.name,
    List<String>? songs,
    this.version = 0,
  }) : songs = songs ?? <String>[];

  Map<String, dynamic> toJson() =>
      {'id': id, 'name': name, 'songs': songs, 'version': version};

  factory Playlist.fromJson(Map<String, dynamic> j) => Playlist(
        id: j['id'] as String,
        name: (j['name'] as String?) ?? '',
        songs: (j['songs'] as List?)?.map((e) => e as String).toList() ??
            <String>[],
        version: (j['version'] as int?) ?? 0,
      );
}

/// Local saved-playlist store. Persists to SharedPreferences and, on every
/// change, publishes the wallet-signed record to DB2 (which floods it to every
/// node). This is the player half of the playlist groundwork.
class PlaylistService extends ChangeNotifier {
  PlaylistService._();
  static final PlaylistService instance = PlaylistService._();

  static const _kKey = 'db2_playlists';
  final List<Playlist> _playlists = <Playlist>[];
  bool _loaded = false;

  List<Playlist> get playlists => List.unmodifiable(_playlists);

  Future<void> ensureLoaded() async {
    if (_loaded) return;
    _loaded = true;
    try {
      final prefs = await SharedPreferences.getInstance();
      final raw = prefs.getString(_kKey);
      if (raw != null) {
        final list = jsonDecode(raw) as List;
        _playlists
          ..clear()
          ..addAll(list.map((e) =>
              Playlist.fromJson((e as Map).cast<String, dynamic>())));
      }
    } catch (_) {/* corrupt prefs — start empty */}
    notifyListeners();
  }

  Future<Playlist> create(String name) async {
    await ensureLoaded();
    final n = name.trim();
    final pl = Playlist(id: _newId(), name: n.isEmpty ? 'Untitled' : n);
    _playlists.add(pl);
    await _afterChange(pl);
    return pl;
  }

  Future<void> rename(Playlist pl, String name) async {
    pl.name = name.trim().isEmpty ? pl.name : name.trim();
    await _afterChange(pl);
  }

  Future<void> addSong(Playlist pl, String contentHash) async {
    if (contentHash.isEmpty || pl.songs.contains(contentHash)) return;
    pl.songs.add(contentHash);
    await _afterChange(pl);
  }

  Future<void> removeSong(Playlist pl, String contentHash) async {
    if (pl.songs.remove(contentHash)) await _afterChange(pl);
  }

  Future<void> reorder(Playlist pl, int oldIndex, int newIndex) async {
    if (oldIndex < 0 || oldIndex >= pl.songs.length) return;
    if (newIndex > oldIndex) newIndex -= 1;
    final h = pl.songs.removeAt(oldIndex);
    pl.songs.insert(newIndex.clamp(0, pl.songs.length), h);
    await _afterChange(pl);
  }

  /// Re-publish every saved playlist (idempotent — the node version-gates
  /// unchanged records). Call on reconnect so edits made while the home node
  /// was unreachable still converge onto the network.
  Future<void> republishAll() async {
    await ensureLoaded();
    for (final pl in List<Playlist>.from(_playlists)) {
      await PlaylistPublisher.publish(pl);
    }
  }

  Future<void> delete(Playlist pl) async {
    _playlists.removeWhere((p) => p.id == pl.id);
    pl.version = DateTime.now().millisecondsSinceEpoch;
    await _persist();
    notifyListeners();
    unawaited(PlaylistPublisher.publish(pl, deleted: true));
  }

  // ---- internals ----------------------------------------------------------

  Future<void> _afterChange(Playlist pl) async {
    pl.version = DateTime.now().millisecondsSinceEpoch;
    await _persist();
    notifyListeners();
    unawaited(PlaylistPublisher.publish(pl));
  }

  Future<void> _persist() async {
    try {
      final prefs = await SharedPreferences.getInstance();
      await prefs.setString(
          _kKey, jsonEncode(_playlists.map((p) => p.toJson()).toList()));
    } catch (_) {}
  }

  String _newId() {
    final r = Random.secure();
    return List<int>.generate(16, (_) => r.nextInt(256))
        .map((x) => x.toRadixString(16).padLeft(2, '0'))
        .join();
  }
}
