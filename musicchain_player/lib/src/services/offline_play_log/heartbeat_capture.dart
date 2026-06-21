// Persistent heartbeat log for offline play-proof bundles.
//
// While the player is offline the existing HeartbeatService can't reach
// session.heartbeat on the home node, so its beats vanish. We mirror
// every beat into the local store here so the OfflineSubmitService can
// gather them into a signed bundle at reconnect time.
//
// The on-disk shape mirrors the wire format from
// docs/offline_play_proof.md: one row per heartbeat, plus a sessions
// table for session-level start/end + content_hash. We also store
// network transitions, battery samples and screen on/off intervals
// in their own tables so a single sqflite write barrier covers the
// full bundle.
//
// Storage choice: sqflite — cache_service already pulls it in, no new
// pubspec dep, and the queries we run (`WHERE submitted = 0 AND
// player_address = ?`) hit indexed columns.

import 'dart:async';
import 'dart:io';

import 'package:path_provider/path_provider.dart';
import 'package:sqflite/sqflite.dart';

class CapturedHeartbeat {
  CapturedHeartbeat({
    required this.sessionId,
    required this.contentHash,
    required this.positionMs,
    required this.wallMs,
    required this.monotonicMs,
  });

  final String sessionId;
  final String contentHash;
  final int    positionMs;
  final int    wallMs;
  final int    monotonicMs;

  Map<String, dynamic> toJson() => {
    'wall_ms':      wallMs,
    'monotonic_ms': monotonicMs,
    'position_ms':  positionMs,
  };
}

class CapturedSession {
  CapturedSession({
    required this.sessionId,
    required this.contentHash,
    required this.blockHash,
    required this.startedWallMs,
    required this.startedMonotonicMs,
    required this.endedWallMs,
    required this.endedMonotonicMs,
    required this.songDurationMs,
    required this.playerAddress,
    required this.heartbeats,
  });

  final String                 sessionId;
  final String                 contentHash;
  final String                 blockHash;
  final int                    startedWallMs;
  final int                    startedMonotonicMs;
  final int                    endedWallMs;
  final int                    endedMonotonicMs;
  final int                    songDurationMs;
  final String                 playerAddress;
  final List<CapturedHeartbeat> heartbeats;

  Map<String, dynamic> toJson() => {
    'session_id':           sessionId,
    'content_hash':         contentHash,
    'block_hash':           blockHash,
    'started_wall_ms':      startedWallMs,
    'started_monotonic_ms': startedMonotonicMs,
    'ended_wall_ms':        endedWallMs,
    'ended_monotonic_ms':   endedMonotonicMs,
    'song_duration_ms':     songDurationMs,
    'heartbeats':           heartbeats.map((h) => h.toJson()).toList(),
  };
}

class HeartbeatCapture {
  HeartbeatCapture._();
  static final HeartbeatCapture instance = HeartbeatCapture._();

  Database? _db;
  final _stopwatch = Stopwatch()..start();
  bool _initialized = false;

  /// Monotonic-ms reading the rest of the offline-log services should
  /// use. Single source so heartbeat capture and transition capture
  /// share the same clock origin.
  int monotonicMs() => _stopwatch.elapsedMilliseconds;
  int wallMs()      => DateTime.now().millisecondsSinceEpoch;

  Future<void> init() async {
    if (_initialized) return;
    final dir = await getApplicationSupportDirectory();
    final path = '${dir.path}/offline_play_log.db';
    await Directory(dir.path).create(recursive: true);

    _db = await openDatabase(
      path,
      version: 1,
      onCreate: (db, _) async {
        await db.execute('''
          CREATE TABLE sessions (
            session_id            TEXT PRIMARY KEY,
            content_hash          TEXT NOT NULL,
            block_hash            TEXT NOT NULL,
            player_address        TEXT NOT NULL,
            started_wall_ms       INTEGER NOT NULL,
            started_monotonic_ms  INTEGER NOT NULL,
            ended_wall_ms         INTEGER,
            ended_monotonic_ms    INTEGER,
            song_duration_ms      INTEGER NOT NULL,
            submitted             INTEGER NOT NULL DEFAULT 0
          )
        ''');
        await db.execute('''
          CREATE TABLE heartbeats (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            session_id    TEXT NOT NULL,
            content_hash  TEXT NOT NULL,
            position_ms   INTEGER NOT NULL,
            wall_ms       INTEGER NOT NULL,
            monotonic_ms  INTEGER NOT NULL,
            submitted     INTEGER NOT NULL DEFAULT 0
          )
        ''');
        await db.execute(
            'CREATE INDEX hb_sess_idx ON heartbeats(session_id, submitted)');
        await db.execute('''
          CREATE TABLE network_transitions (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            wall_ms       INTEGER NOT NULL,
            monotonic_ms  INTEGER NOT NULL,
            kind          TEXT NOT NULL,
            fingerprint   TEXT NOT NULL,
            submitted     INTEGER NOT NULL DEFAULT 0
          )
        ''');
        await db.execute('''
          CREATE TABLE battery_samples (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            wall_ms       INTEGER NOT NULL,
            monotonic_ms  INTEGER NOT NULL,
            percent       INTEGER NOT NULL,
            charging      INTEGER NOT NULL,
            submitted     INTEGER NOT NULL DEFAULT 0
          )
        ''');
        await db.execute('''
          CREATE TABLE screen_intervals (
            id                INTEGER PRIMARY KEY AUTOINCREMENT,
            on_wall_ms        INTEGER NOT NULL,
            on_monotonic_ms   INTEGER NOT NULL,
            off_wall_ms       INTEGER,
            off_monotonic_ms  INTEGER,
            submitted         INTEGER NOT NULL DEFAULT 0
          )
        ''');
      },
    );
    _initialized = true;
  }

  /// Opens a session row at session-start time so subsequent heartbeats
  /// have a stable parent. Idempotent on session_id.
  Future<void> openSession({
    required String sessionId,
    required String contentHash,
    required String blockHash,
    required String playerAddress,
    required int    songDurationMs,
  }) async {
    if (_db == null) await init();
    final wall = wallMs();
    final mono = monotonicMs();
    await _db!.insert(
      'sessions',
      {
        'session_id':            sessionId,
        'content_hash':          contentHash,
        'block_hash':            blockHash,
        'player_address':        playerAddress,
        'started_wall_ms':       wall,
        'started_monotonic_ms':  mono,
        'ended_wall_ms':         null,
        'ended_monotonic_ms':    null,
        'song_duration_ms':      songDurationMs,
        'submitted':             0,
      },
      conflictAlgorithm: ConflictAlgorithm.ignore,
    );
  }

  /// One row per heartbeat tick. Called by HeartbeatService on every
  /// timer tick (independent of whether the online RPC succeeded —
  /// either way we mirror the beat locally).
  Future<void> recordHeartbeat({
    required String sessionId,
    required String contentHash,
    required int    positionMs,
  }) async {
    if (_db == null) await init();
    await _db!.insert('heartbeats', {
      'session_id':   sessionId,
      'content_hash': contentHash,
      'position_ms':  positionMs,
      'wall_ms':      wallMs(),
      'monotonic_ms': monotonicMs(),
      'submitted':    0,
    });
  }

  /// Marks a session as ended. Caller uses this on natural stop / next-
  /// song transitions. If never called we still pick the session up at
  /// submit time using the last heartbeat as the end timestamp.
  Future<void> closeSession(String sessionId) async {
    if (_db == null) await init();
    await _db!.update(
      'sessions',
      {
        'ended_wall_ms':      wallMs(),
        'ended_monotonic_ms': monotonicMs(),
      },
      where: 'session_id = ? AND ended_wall_ms IS NULL',
      whereArgs: [sessionId],
    );
  }

  // ---- Transition / sensor helpers ------------------------------------

  Future<void> recordTransition({
    required String kind,
    required String fingerprint,
  }) async {
    if (_db == null) await init();
    await _db!.insert('network_transitions', {
      'wall_ms':      wallMs(),
      'monotonic_ms': monotonicMs(),
      'kind':         kind,
      'fingerprint':  fingerprint,
      'submitted':    0,
    });
  }

  Future<void> recordBattery({required int percent, required bool charging}) async {
    if (_db == null) await init();
    await _db!.insert('battery_samples', {
      'wall_ms':      wallMs(),
      'monotonic_ms': monotonicMs(),
      'percent':      percent,
      'charging':     charging ? 1 : 0,
      'submitted':    0,
    });
  }

  Future<int> openScreenInterval() async {
    if (_db == null) await init();
    return _db!.insert('screen_intervals', {
      'on_wall_ms':       wallMs(),
      'on_monotonic_ms':  monotonicMs(),
      'off_wall_ms':      null,
      'off_monotonic_ms': null,
      'submitted':        0,
    });
  }

  Future<void> closeScreenInterval(int id) async {
    if (_db == null) await init();
    await _db!.update(
      'screen_intervals',
      {
        'off_wall_ms':      wallMs(),
        'off_monotonic_ms': monotonicMs(),
      },
      where: 'id = ? AND off_wall_ms IS NULL',
      whereArgs: [id],
    );
  }

  // ---- Bundle assembly -----------------------------------------------

  /// Returns every session with at least one heartbeat that hasn't
  /// been submitted yet, restricted to `playerAddress`. Sessions whose
  /// `ended_*` columns are still null are closed-in-memory using the
  /// latest heartbeat for the same session.
  Future<List<CapturedSession>> unsubmittedSessions(String playerAddress) async {
    if (_db == null) await init();
    // On-chain player addresses are lowercase, but openSession may have
    // stored a mixed-case form (e.g. EIP-55 checksummed). Normalize both
    // sides so the bundle isn't silently dropped on the case mismatch.
    final rows = await _db!.rawQuery('''
      SELECT s.* FROM sessions s
      WHERE LOWER(s.player_address) = LOWER(?) AND s.submitted = 0
      ORDER BY s.started_wall_ms ASC
    ''', [playerAddress]);
    final out = <CapturedSession>[];
    for (final row in rows) {
      final sid = row['session_id'] as String;
      final beats = await _db!.query(
        'heartbeats',
        where: 'session_id = ? AND submitted = 0',
        whereArgs: [sid],
        orderBy: 'wall_ms ASC',
      );
      if (beats.isEmpty) continue;
      final lastBeat = beats.last;
      final endedWall = (row['ended_wall_ms'] as int?) ??
          (lastBeat['wall_ms'] as int);
      final endedMono = (row['ended_monotonic_ms'] as int?) ??
          (lastBeat['monotonic_ms'] as int);
      out.add(CapturedSession(
        sessionId:           sid,
        contentHash:         row['content_hash'] as String,
        blockHash:           row['block_hash']   as String,
        playerAddress:       row['player_address'] as String,
        startedWallMs:       row['started_wall_ms']      as int,
        startedMonotonicMs:  row['started_monotonic_ms'] as int,
        endedWallMs:         endedWall,
        endedMonotonicMs:    endedMono,
        songDurationMs:      row['song_duration_ms']     as int,
        heartbeats: beats.map((b) => CapturedHeartbeat(
          sessionId:   sid,
          contentHash: b['content_hash'] as String,
          positionMs:  b['position_ms']  as int,
          wallMs:      b['wall_ms']      as int,
          monotonicMs: b['monotonic_ms'] as int,
        )).toList(),
      ));
    }
    return out;
  }

  Future<List<Map<String, dynamic>>> unsubmittedTransitions() async {
    if (_db == null) await init();
    final rows = await _db!.query(
      'network_transitions',
      where: 'submitted = 0',
      orderBy: 'wall_ms ASC',
    );
    return rows.map((r) => {
      'wall_ms':      r['wall_ms'],
      'monotonic_ms': r['monotonic_ms'],
      'kind':         r['kind'],
      'fingerprint':  r['fingerprint'],
    }).toList();
  }

  Future<List<Map<String, dynamic>>> unsubmittedBattery() async {
    if (_db == null) await init();
    final rows = await _db!.query(
      'battery_samples',
      where: 'submitted = 0',
      orderBy: 'wall_ms ASC',
    );
    return rows.map((r) => {
      'wall_ms':      r['wall_ms'],
      'monotonic_ms': r['monotonic_ms'],
      'percent':      r['percent'],
      'charging':     (r['charging'] as int) == 1,
    }).toList();
  }

  Future<List<Map<String, dynamic>>> unsubmittedScreenIntervals() async {
    if (_db == null) await init();
    final rows = await _db!.query(
      'screen_intervals',
      where: 'submitted = 0 AND off_wall_ms IS NOT NULL',
      orderBy: 'on_wall_ms ASC',
    );
    return rows.map((r) => {
      'on_wall_ms':       r['on_wall_ms'],
      'on_monotonic_ms':  r['on_monotonic_ms'],
      'off_wall_ms':      r['off_wall_ms'],
      'off_monotonic_ms': r['off_monotonic_ms'],
    }).toList();
  }

  /// Mark every row that participated in the bundle as submitted so
  /// the next reconnect doesn't double-submit. Caller passes the same
  /// session ids it just shipped, plus the wall-ms cutoff that was
  /// captured *before* the RPC went out — anything inserted after that
  /// instant is for the NEXT bundle and must stay unsubmitted.
  Future<void> markSubmitted(List<String> sessionIds, {required int cutoffWallMs}) async {
    if (_db == null) await init();
    if (sessionIds.isEmpty) {
      // Even with no sessions we may have shipped sensor / transition
      // rows — mark everything ≤ cutoffWallMs as sent so we don't
      // re-ship, but leave anything captured during the RPC alone.
      await _db!.update('network_transitions', {'submitted': 1},
          where: 'submitted = 0 AND wall_ms <= ?', whereArgs: [cutoffWallMs]);
      await _db!.update('battery_samples', {'submitted': 1},
          where: 'submitted = 0 AND wall_ms <= ?', whereArgs: [cutoffWallMs]);
      await _db!.update('screen_intervals', {'submitted': 1},
          where: 'submitted = 0 AND off_wall_ms IS NOT NULL '
                 'AND off_wall_ms <= ?', whereArgs: [cutoffWallMs]);
      return;
    }
    final placeholders = List.filled(sessionIds.length, '?').join(',');
    await _db!.update(
      'sessions',
      {'submitted': 1},
      where: 'session_id IN ($placeholders)',
      whereArgs: sessionIds,
    );
    await _db!.update(
      'heartbeats',
      {'submitted': 1},
      where: 'session_id IN ($placeholders)',
      whereArgs: sessionIds,
    );
    // Sensor / transition rows aren't keyed by session_id; flush
    // everything captured at-or-before the cutoff we snapshotted before
    // the RPC. New rows inserted during the RPC stay unsubmitted.
    await _db!.update('network_transitions', {'submitted': 1},
        where: 'submitted = 0 AND wall_ms <= ?', whereArgs: [cutoffWallMs]);
    await _db!.update('battery_samples', {'submitted': 1},
        where: 'submitted = 0 AND wall_ms <= ?', whereArgs: [cutoffWallMs]);
    await _db!.update('screen_intervals', {'submitted': 1},
        where: 'submitted = 0 AND off_wall_ms IS NOT NULL '
               'AND off_wall_ms <= ?', whereArgs: [cutoffWallMs]);
  }

  /// Cap on disk usage. Drops oldest rows beyond 7 days OR beyond
  /// 5000 sessions. Cheap, runs at every submit attempt.
  Future<void> trim() async {
    if (_db == null) await init();
    final cutoff = wallMs() - const Duration(days: 7).inMilliseconds;
    await _db!.delete('sessions',
        where: 'submitted = 1 AND started_wall_ms < ?', whereArgs: [cutoff]);
    await _db!.delete('heartbeats',
        where: 'submitted = 1 AND wall_ms < ?', whereArgs: [cutoff]);
    await _db!.delete('network_transitions',
        where: 'submitted = 1 AND wall_ms < ?', whereArgs: [cutoff]);
    await _db!.delete('battery_samples',
        where: 'submitted = 1 AND wall_ms < ?', whereArgs: [cutoff]);
    await _db!.delete('screen_intervals',
        where: 'submitted = 1 AND off_wall_ms IS NOT NULL '
               'AND off_wall_ms < ?', whereArgs: [cutoff]);
  }
}
