import 'dart:io';
import 'package:path_provider/path_provider.dart';
import 'package:sqflite/sqflite.dart';

class CacheService {
  Database? _db;
  String?   _cacheDir;
  int       _maxBytes;

  CacheService({int maxMb = 1000}) : _maxBytes = maxMb * 1024 * 1024;

  Future<void> init() async {
    final appDir   = await getApplicationSupportDirectory();
    _cacheDir = '${appDir.path}/cache';
    await Directory(_cacheDir!).create(recursive: true);

    _db = await openDatabase(
      '${appDir.path}/cache_index.db',
      version: 1,
      onCreate: (db, _) async {
        await db.execute('''
          CREATE TABLE cache_entries (
            content_hash TEXT PRIMARY KEY,
            block_hash   TEXT,
            file_path    TEXT,
            size_bytes   INTEGER,
            cached_at    INTEGER
          )
        ''');
      },
    );
  }

  // Returns cached Ogg data path, or null if not cached
  Future<String?> getCachedPath(String contentHash) async {
    final rows = await _db!.query(
      'cache_entries',
      where: 'content_hash = ?',
      whereArgs: [contentHash],
    );
    if (rows.isEmpty) return null;
    final path = rows.first['file_path'] as String;
    if (await File(path).exists()) return path;
    // Stale entry — clean up
    await _db!.delete('cache_entries', where: 'content_hash = ?', whereArgs: [contentHash]);
    return null;
  }

  // Save audio data to cache, returns local path
  Future<String> save(String contentHash, String blockHash, List<int> data) async {
    // Enforce size limit
    await _evictIfNeeded(data.length);

    final prefix = contentHash.substring(0, 8);
    final dir    = Directory('$_cacheDir/$prefix');
    await dir.create(recursive: true);
    final path = '${dir.path}/$contentHash.ogg';
    await File(path).writeAsBytes(data);

    await _db!.insert(
      'cache_entries',
      {
        'content_hash': contentHash,
        'block_hash':   blockHash,
        'file_path':    path,
        'size_bytes':   data.length,
        'cached_at':    DateTime.now().millisecondsSinceEpoch,
      },
      conflictAlgorithm: ConflictAlgorithm.replace,
    );
    return path;
  }

  Future<void> clear() async {
    final rows = await _db!.query('cache_entries');
    for (final row in rows) {
      final path = row['file_path'] as String;
      try { await File(path).delete(); } catch (_) {}
    }
    await _db!.delete('cache_entries');
  }

  Future<Map<String, dynamic>> stats() async {
    final rows = await _db!.query('cache_entries');
    int totalBytes = 0;
    for (final row in rows) totalBytes += (row['size_bytes'] as int);
    return {'count': rows.length, 'total_bytes': totalBytes};
  }

  Future<void> _evictIfNeeded(int incomingBytes) async {
    final rows = await _db!.query('cache_entries', orderBy: 'cached_at ASC');
    int totalBytes = 0;
    for (final row in rows) totalBytes += (row['size_bytes'] as int);

    while (totalBytes + incomingBytes > _maxBytes && rows.isNotEmpty) {
      final oldest = rows.removeAt(0);
      final path   = oldest['file_path'] as String;
      totalBytes  -= oldest['size_bytes'] as int;
      try { await File(path).delete(); } catch (_) {}
      await _db!.delete('cache_entries',
          where: 'content_hash = ?', whereArgs: [oldest['content_hash']]);
    }
  }
}
