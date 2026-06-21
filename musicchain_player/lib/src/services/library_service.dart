// Local library tracking + persistence.
//
// One row per song the player has fingerprinted locally (or downloaded
// from another wallet's library). Indexed by `content_hash` (sha256 of
// the raw audio bytes). The folder list lives alongside so the
// background scanner knows where to look for new files.
//
// State is JSON-encoded into SharedPreferences. Volume is small enough
// (~1 KB per song) that this is fine for tens of thousands of entries.

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

class LibraryEntry {
  LibraryEntry({
    required this.contentHash,
    required this.fingerprintHash,
    this.canonicalHash = '',
    required this.title,
    required this.artist,
    required this.album,
    required this.genre,
    this.year = 0,
    this.trackNumber = 0,
    this.bitrate = 0,
    required this.durationMs,
    required this.audioFormat,
    required this.filePath,
    required this.addedAtMs,
  });

  /// Hash of THIS local file's bytes. PlayerServer uses this to find the
  /// file when a peer asks stream.open(this_hash).
  final String contentHash;
  final String fingerprintHash;
  /// The canonical chain content_hash for the song this file is a
  /// variant of. Equal to [contentHash] when this player was the song's
  /// first submitter (no fuzzy-matched variants). Filled in by
  /// downloadToLibrary so the chain library "local" badge can find our
  /// downloaded copy by the chain's canonical hash.
  String canonicalHash;
  String title;
  String artist;
  String album;
  String genre;
  int    year;        // 4-digit release year, 0 = unknown
  int    trackNumber; // 0 = unknown
  int    bitrate;     // bits/sec, 0 = unknown — drives variant picker
  int    durationMs;
  String audioFormat;  // 'ogg' | 'mp3'
  String filePath;     // empty for entries downloaded from peers
  int    addedAtMs;

  bool get isLocal => filePath.isNotEmpty && File(filePath).existsSync();

  Map<String, dynamic> toJson() => {
        'content_hash':     contentHash,
        'fingerprint_hash': fingerprintHash,
        'canonical_hash':   canonicalHash,
        'title':            title,
        'artist':           artist,
        'album':            album,
        'genre':            genre,
        'year':             year,
        'track_number':     trackNumber,
        'bitrate':          bitrate,
        'duration_ms':      durationMs,
        'audio_format':     audioFormat,
        'file_path':        filePath,
        'added_at_ms':      addedAtMs,
      };

  static LibraryEntry fromJson(Map<String, dynamic> j) => LibraryEntry(
        contentHash:     j['content_hash']     as String,
        fingerprintHash: j['fingerprint_hash'] as String? ?? '',
        canonicalHash:   j['canonical_hash']   as String? ?? '',
        title:           j['title']            as String? ?? '',
        artist:          j['artist']           as String? ?? '',
        album:           j['album']            as String? ?? '',
        genre:           j['genre']            as String? ?? '',
        year:            j['year']             as int?    ?? 0,
        trackNumber:     j['track_number']     as int?    ?? 0,
        bitrate:         j['bitrate']          as int?    ?? 0,
        durationMs:      j['duration_ms']      as int?    ?? 0,
        audioFormat:     j['audio_format']     as String? ?? 'mp3',
        filePath:        j['file_path']        as String? ?? '',
        addedAtMs:       j['added_at_ms']      as int?    ?? 0,
      );
}

class LibraryService extends ChangeNotifier {
  LibraryService._();
  static final LibraryService instance = LibraryService._();

  final Map<String, LibraryEntry> _byHash      = {};
  final Map<String, LibraryEntry> _byPath      = {};
  final Map<String, LibraryEntry> _byCanonical = {};
  final List<String>              _folders = [];
  bool _loaded = false;
  Future<void>? _loading;

  static const _kFoldersKey = 'mc_library_folders';
  static const _kEntriesKey = 'mc_library_entries';

  Future<void> ensureLoaded() {
    if (_loaded) return Future.value();
    // Cache the in-flight load so concurrent callers await the same
    // future instead of each re-reading prefs and re-indexing entries.
    // Without this, a second ensureLoaded() racing with the first could
    // clobber entries that an upsert() inserted between the two reads.
    return _loading ??= _doLoad();
  }

  Future<void> _doLoad() async {
    try {
      final prefs = await SharedPreferences.getInstance();
      _folders
        ..clear()
        ..addAll(prefs.getStringList(_kFoldersKey) ?? const []);
      final raw = prefs.getString(_kEntriesKey);
      if (raw != null && raw.isNotEmpty) {
        try {
          final list = (jsonDecode(raw) as List).cast<dynamic>();
          for (final item in list) {
            final e = LibraryEntry.fromJson(
                (item as Map).cast<String, dynamic>());
            _index(e);
          }
        } catch (_) { /* corrupt prefs: start empty */ }
      }
      _loaded = true;
      notifyListeners();
    } finally {
      _loading = null;
    }
  }

  /// Insert / update [e] in all three lookup maps (hash, canonical, path).
  /// Used by [ensureLoaded] and [upsert] so every public read path is O(1).
  void _index(LibraryEntry e) {
    _byHash[e.contentHash] = e;
    if (e.canonicalHash.isNotEmpty) _byCanonical[e.canonicalHash] = e;
    if (e.filePath.isNotEmpty)       _byPath[e.filePath]          = e;
  }

  void _unindex(LibraryEntry e) {
    _byHash.remove(e.contentHash);
    if (e.canonicalHash.isNotEmpty) _byCanonical.remove(e.canonicalHash);
    if (e.filePath.isNotEmpty)       _byPath.remove(e.filePath);
  }

  List<String> get folders => List.unmodifiable(_folders);
  List<LibraryEntry> get entries => List.unmodifiable(_byHash.values);

  /// O(1) lookup by either content_hash OR canonical_hash. Used by the
  /// chain library's "local" badge and by playback to find a local file
  /// when the caller only knows the canonical hash.
  LibraryEntry? entryByHash(String hash) =>
      _byHash[hash] ?? _byCanonical[hash];

  /// O(1) lookup by absolute file path. Used by the scanner to skip
  /// already-known files without re-reading their bytes — the old
  /// O(N) `entries.firstWhere` was making a force-rescan of a large
  /// library quadratic in time.
  LibraryEntry? entryByPath(String filePath) => _byPath[filePath];

  Future<void> addFolder(String path) async {
    if (_folders.contains(path)) return;
    _folders.add(path);
    await _persistFolders();
    notifyListeners();
  }

  Future<void> removeFolder(String path) async {
    if (!_folders.remove(path)) return;
    await _persistFolders();
    notifyListeners();
  }

  /// All library entries whose `filePath` sits under [folderPath]. Used
  /// by the UI to surface "this many songs will be removed" before the
  /// user confirms, and by [purgeFolder] to know which entries to
  /// deannounce.
  List<LibraryEntry> entriesUnder(String folderPath) {
    final norm = _normalizeFolder(folderPath);
    return _byHash.values
        .where((e) => e.filePath.isNotEmpty &&
                      _normalizePath(e.filePath).startsWith(norm))
        .toList();
  }

  /// Drop every entry whose `filePath` lives under [folderPath], remove
  /// [folderPath] from the folder list, and return the list of entries
  /// that were dropped so the caller can deannounce them from the swarm.
  Future<List<LibraryEntry>> purgeFolder(String folderPath) async {
    final affected = entriesUnder(folderPath);
    for (final e in affected) {
      _unindex(e);
    }
    _folders.remove(folderPath);
    await _persistFolders();
    await _persistEntries();
    notifyListeners();
    return affected;
  }

  /// Best-effort path-prefix check: normalize separators and lowercase
  /// the drive letter on Windows so `C:\Music` vs `c:\Music\Sub\file.mp3`
  /// still match.
  static String _normalizePath(String p) {
    final unified = p.replaceAll('\\', '/');
    if (unified.length >= 2 && unified[1] == ':') {
      return unified[0].toLowerCase() + unified.substring(1);
    }
    return unified;
  }
  static String _normalizeFolder(String p) {
    var f = _normalizePath(p);
    if (!f.endsWith('/')) f = '$f/';
    return f;
  }

  Future<void> upsert(LibraryEntry entry) async {
    // If this content_hash existed already with a different path/
    // canonical, scrub the old indexes first so they don't leak.
    final prior = _byHash[entry.contentHash];
    if (prior != null) _unindex(prior);
    _index(entry);
    await _persistEntries();
    notifyListeners();
  }

  Future<void> remove(String contentHash) async {
    final prior = _byHash[contentHash];
    if (prior == null) return;
    _unindex(prior);
    await _persistEntries();
    notifyListeners();
  }

  Future<void> _persistFolders() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setStringList(_kFoldersKey, _folders);
  }

  Future<void> _persistEntries() async {
    final prefs = await SharedPreferences.getInstance();
    final list = _byHash.values.map((e) => e.toJson()).toList();
    await prefs.setString(_kEntriesKey, jsonEncode(list));
  }
}
