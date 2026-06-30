import 'dart:async';

import 'package:flutter/foundation.dart';

import '../models/song.dart';
import '../services/librats_discovery.dart';
import '../services/node_client.dart';
import '../services/node_service.dart';
import '../services/rats_client.dart';

class LibraryProvider extends ChangeNotifier {
  final NodeClient _client = NodeClient();

  List<Song> songs    = [];
  bool       loading  = false;
  String?    error;
  String     filter   = '';

  /// In-flight refresh/search future. Concurrent callers (timer + user
  /// tap + discovery callback) await this instead of issuing a parallel
  /// RPC; without it the LibraryScreen's 20s timer firing during a slow
  /// search would race the search reply and stomp `songs` with stale
  /// chain data.
  Future<void>? _inFlight;

  List<Song> get filteredSongs {
    // Hide songs the swarm currently can't serve (no peer announced for
    // them) — without this the chain library shows ghost rows that fail
    // to stream. Also dedup by content_hash defensively in case the
    // full node ever returns the same row twice.
    final seen = <String>{};
    final live = <Song>[];
    for (final s in songs) {
      if (s.swarmSize <= 0) continue;
      if (!seen.add(s.contentHash)) continue;
      live.add(s);
    }
    if (filter.isEmpty) return live;
    final q = filter.toLowerCase();
    return live.where((s) =>
      s.title.toLowerCase().contains(q) ||
      s.artist.toLowerCase().contains(q) ||
      s.genre.toLowerCase().contains(q) ||
      s.album.toLowerCase().contains(q)
    ).toList();
  }

  /// Songs grouped by album, preserving insertion order. Songs without an
  /// album fall under "(Singles)". Each album's track list is itself ordered
  /// the same way the node returned the songs (typically chronological).
  Map<String, List<Song>> get albums {
    const noAlbumKey = '(Singles)';
    final out = <String, List<Song>>{};
    for (final s in filteredSongs) {
      final key = s.album.trim().isEmpty ? noAlbumKey : s.album.trim();
      out.putIfAbsent(key, () => []).add(s);
    }
    return out;
  }

  Future<NodeClient> _getClient() async {
    final pid = await NodeService.getRatsPeerId();
    if (pid.isEmpty) {
      throw Exception('No node discovered yet. Open Settings to refresh.');
    }
    _client.ratsPeerId = pid;
    return _client;
  }

  /// Run [op] against a fresh NodeClient. If the call comes back with a
  /// `timeout` / `send_failed` RatsRpcException, the auto-selected full
  /// node almost certainly cycled (restart, NAT flap, VPS hand-off) —
  /// kick LibratsDiscovery for a fresh routes table and retry exactly
  /// once before bubbling the error up.
  Future<T> _withRediscoverRetry<T>(
      Future<T> Function(NodeClient c) op) async {
    try {
      return await op(await _getClient());
    } on RatsRpcException catch (e) {
      if (e.status != 'timeout' && e.status != 'send_failed') rethrow;
      final disc = LibratsDiscovery.current;
      if (disc == null) rethrow;
      await disc.refresh();
      // After a rediscover the VPS handshake may still be mid-flight
      // (LibratsDiscovery sets prefs as soon as routes.get returns, but
      // routing through setRelayVia + the librats validate-peer set
      // can lag a few hundred ms). Without this gap the retry fires
      // straight into the same send_failed because validatedPeerIds
      // is still empty. The wait inside RatsClient.request handles
      // first-launch cold start, but a refresh-mid-session needs this
      // extra slack on top.
      final rats = RatsClient.instance;
      final deadline = DateTime.now().add(const Duration(seconds: 2));
      while (rats.validatedPeerIds.isEmpty &&
             DateTime.now().isBefore(deadline)) {
        await Future<void>.delayed(const Duration(milliseconds: 100));
      }
      return await op(await _getClient());
    }
  }

  Future<void> _runSingle(Future<List<Song>> Function() body) {
    final existing = _inFlight;
    if (existing != null) return existing;
    final c = Completer<void>();
    _inFlight = c.future;
    () async {
      loading = true;
      error   = null;
      notifyListeners();
      try {
        songs = await body();
      } catch (e) {
        error = e.toString();
      }
      loading = false;
      notifyListeners();
      _inFlight = null;
      c.complete();
    }();
    return c.future;
  }

  Future<void> refresh() =>
      _runSingle(() => _withRediscoverRetry((c) => c.getSongs()));

  Future<void> search(String query) =>
      _runSingle(() => _withRediscoverRetry((c) => c.searchSongs(query)));

  Future<void> searchByArtist(String artist) =>
      _runSingle(() => _withRediscoverRetry((c) => c.searchSongsByArtist(artist)));

  Future<void> searchByGenre(String genre) =>
      _runSingle(() => _withRediscoverRetry((c) => c.searchSongsByGenre(genre)));

  void setFilter(String value) {
    filter = value;
    notifyListeners();
  }
}
