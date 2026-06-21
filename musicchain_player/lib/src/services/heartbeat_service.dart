import 'dart:async';

import 'node_client.dart';
import 'offline_play_log/heartbeat_capture.dart';

/// Posts session.heartbeat to the full node on a fixed 5 s cadence while
/// the player is actively playing the current song. Pause / stop / load
/// transitions silently skip the tick so the full node's union-of-
/// timestamp-ranges play-count math (server.cpp post_session_complete)
/// doesn't synthesize listen credit for a song that's been parked at
/// the same position for minutes.
///
/// The payload is intentionally minimal — `{session_id, position_ms}`.
/// The server uses its own wall clock and ignores any sender-supplied
/// timestamp, and the old PCM-checksum field was never consumed by the
/// full node. Stripping both saves a per-beat allocation + an FFI hop.
class HeartbeatService {
  HeartbeatService(this._client);

  final NodeClient   _client;
  String?            _sessionId;
  String?            _contentHash;
  Timer?             _timer;
  int Function()?    _getPositionMs;
  bool Function()?   _isPlaying;

  /// [contentHash], [blockHash], [playerAddress] and [songDurationMs] are
  /// mirrored into the persistent offline play log so the bundle the
  /// player ships on reconnect can replay the session even if every
  /// online `session.heartbeat` call during this playback failed. None
  /// are required for the original online code path.
  void start(String sessionId,
             int  Function() getPositionMs,
             {required bool Function() isPlaying,
              String contentHash    = '',
              String blockHash      = '',
              String playerAddress  = '',
              int    songDurationMs = 0}) {
    // Defensive: cancel any pre-existing timer so a start() called
    // without an intervening stop() (e.g. song-skip on the player UI)
    // doesn't leak a Timer that keeps firing against the new session.
    _timer?.cancel();
    _sessionId     = sessionId;
    _contentHash   = contentHash;
    _getPositionMs = getPositionMs;
    _isPlaying     = isPlaying;

    // Mirror the session opening into the persistent log so a crash /
    // immediate kill still leaves a recoverable row. Fire-and-forget.
    if (contentHash.isNotEmpty && playerAddress.isNotEmpty) {
      unawaited(HeartbeatCapture.instance.openSession(
        sessionId:      sessionId,
        contentHash:    contentHash,
        blockHash:      blockHash,
        playerAddress:  playerAddress,
        songDurationMs: songDurationMs,
      ));
    }

    // 5 s cadence: dense enough that the union-of-ranges math sees
    // every distinct chunk of the song as the listener moves through
    // it, sparse enough that a marginal-network player still keeps up.
    _timer = Timer.periodic(const Duration(seconds: 5), (_) => _tick());
  }

  Future<void> _tick() async {
    if (_sessionId == null || _getPositionMs == null) return;
    if (_isPlaying != null && !_isPlaying!()) return;
    final pos = _getPositionMs!();
    // Always mirror to the persistent offline log — regardless of
    // whether the online RPC succeeds. The log is the source of truth
    // for offline-play-proof bundles.
    final ch = _contentHash ?? '';
    if (ch.isNotEmpty) {
      unawaited(HeartbeatCapture.instance.recordHeartbeat(
        sessionId:   _sessionId!,
        contentHash: ch,
        positionMs:  pos,
      ));
    }
    try {
      await _client.sendHeartbeat(_sessionId!, positionMs: pos);
    } catch (_) {
      // Swallow heartbeat errors; will retry on the next tick.
    }
  }

  void stop() {
    _timer?.cancel();
    final sid = _sessionId;
    if (sid != null) {
      unawaited(HeartbeatCapture.instance.closeSession(sid));
    }
    _timer         = null;
    _sessionId     = null;
    _contentHash   = null;
    _getPositionMs = null;
    _isPlaying     = null;
  }
}
