import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:media_kit/media_kit.dart';

import '../models/song.dart';
import '../models/session.dart';
import '../services/node_client.dart';
import '../services/node_service.dart';
import '../services/heartbeat_service.dart';
import 'wallet_provider.dart';

enum PlayerState { idle, loading, playing, paused, stopped }

class PlayerProvider extends ChangeNotifier {
  final NodeClient       _client    = NodeClient();
  late final HeartbeatService _heartbeat;
  final Player           _player    = Player();

  PlayerProvider() {
    // Share the same NodeClient so heartbeats use the same resolved URL
    _heartbeat = HeartbeatService(_client);
    _player.stream.playing.listen((playing) {
      // Ignore stream events while loading (open() races with play()), and
      // while the player is in a terminal state owned by us (stopped/idle).
      // Otherwise mpv emitting `playing: false` right after stop()/complete()
      // would flip our state back to `paused` and resurrect a session that
      // we just tore down.
      if (state == PlayerState.loading ||
          state == PlayerState.stopped ||
          state == PlayerState.idle) {
        return;
      }
      state = playing ? PlayerState.playing : PlayerState.paused;
      notifyListeners();
    });
    _player.stream.position.listen((pos) {
      positionMs = pos.inMilliseconds;
      notifyListeners();
    });
    _player.stream.completed.listen((completed) {
      if (completed) _onComplete();
    });
  }

  Song?         currentSong;
  PlaySession?  currentSession;
  PlayerState   state        = PlayerState.idle;
  int           positionMs   = 0;
  String?       errorMessage;

  // Playlist / queue
  List<Song>    playlist     = [];
  int           _playlistIdx = -1;
  String        _playerAddr  = '';

  Future<NodeClient> _getClient() async {
    final pid = await NodeService.getRatsPeerId();
    if (pid.isEmpty) {
      throw Exception('No node discovered yet. Open Settings to refresh.');
    }
    _client.ratsPeerId = pid;
    return _client;
  }

  // ---- Public API ---------------------------------------------------

  Future<void> play(Song song, String playerAddress) async {
    playlist     = [song];
    _playlistIdx = 0;
    _playerAddr  = playerAddress;
    await _playSong(song, playerAddress);
  }

  Future<void> playPlaylist(List<Song> songs, int startIndex, String playerAddress) async {
    playlist     = List.of(songs);
    _playlistIdx = startIndex.clamp(0, songs.length - 1);
    _playerAddr  = playerAddress;
    await _playSong(songs[_playlistIdx], playerAddress);
  }

  Future<void> playNext() async {
    if (playlist.isEmpty) return;
    _playlistIdx = (_playlistIdx + 1) % playlist.length;
    await _playSong(playlist[_playlistIdx], _playerAddr);
  }

  Future<void> playPrev() async {
    if (playlist.isEmpty) return;
    _playlistIdx = (_playlistIdx - 1 + playlist.length) % playlist.length;
    await _playSong(playlist[_playlistIdx], _playerAddr);
  }

  void pause() {
    _player.pause();
    state = PlayerState.paused;
    notifyListeners();
  }

  void resume() {
    _player.play();
    state = PlayerState.playing;
    notifyListeners();
  }

  void stop() {
    final finishedId = currentSession?.sessionId;
    if (finishedId != null) {
      // User-initiated stop counts as a session end too — the home
      // node still checks the 50% rule, so a short stop mid-song
      // won't fraudulently mint anything.
      unawaited(_completeSessionSilently(finishedId));
    }
    _player.stop();
    _heartbeat.stop();
    state          = PlayerState.stopped;
    currentSong    = null;
    currentSession = null;
    positionMs     = 0;
    notifyListeners();
  }

  void togglePlayPause() {
    if (state == PlayerState.playing) {
      pause();
    } else if (state == PlayerState.paused) {
      resume();
    }
  }

  void updatePosition(int ms) {
    positionMs = ms;
    notifyListeners();
  }

  /// Seek the underlying media to [ms]. The heartbeat thread picks up
  /// the new position on its next tick (5 s cadence) — the full node
  /// uses that delta vs the wall-clock delta to decide whether to count
  /// the post-seek interval toward the session's effective listen time.
  Future<void> seek(int ms) async {
    final clamped = ms.clamp(0, currentSong?.durationMs ?? ms);
    await _player.seek(Duration(milliseconds: clamped));
    positionMs = clamped;
    notifyListeners();
  }

  Future<void> seekRelative(int deltaMs) =>
      seek(positionMs + deltaMs);

  Future<MintResult?> complete() async {
    if (currentSession == null) return null;
    try {
      final result = await (await _getClient()).completeSession(currentSession!.sessionId);
      _heartbeat.stop();
      currentSession = null;
      state          = PlayerState.idle;
      notifyListeners();
      WalletProvider.refreshNow();
      return result;
    } catch (_) {
      return null;
    }
  }

  /// Fire-and-forget session.complete used by the implicit-finish paths
  /// (track ended naturally, user skipped, user stopped). Swallows errors
  /// so a slow / unreachable full node doesn't block the UI from queueing
  /// the next track. The full node's 50% effective-listen check is what
  /// decides whether this turns into a MintTx, so spamming completes for
  /// short skips can't inflate play counts.
  Future<void> _completeSessionSilently(String sessionId) async {
    try {
      await (await _getClient()).completeSession(sessionId);
      // Nudge the wallet to re-fetch — completeSession is the moment
      // the chain mints the discoverer + node + artist-escrow credits,
      // so the cached balance the wallet tab shows is stale RIGHT
      // NOW. Static refresh avoids hauling BuildContext through the
      // player stack.
      WalletProvider.refreshNow();
    } catch (_) {/* best effort */}
  }

  // ---- Internal -----------------------------------------------------

  Future<void> _playSong(Song song, String playerAddress) async {
    // Finalize whatever session was active before tearing down the
    // local player. Skip-next / skip-prev would otherwise leave the
    // previous song's session orphaned in the full node's map.
    final priorId = currentSession?.sessionId;
    if (priorId != null) {
      unawaited(_completeSessionSilently(priorId));
      currentSession = null;
    }
    await _player.stop();
    state        = PlayerState.loading;
    errorMessage = null;
    notifyListeners();

    try {
      // Resolve node URL before starting session
      await _getClient();

      // Start play session
      final PlaySession session;
      try {
        session = await _client.startSession(song.contentHash, playerAddress);
      } catch (e) {
        final msg = e.toString();
        errorMessage = msg.contains('402')
            ? 'You need at least 1 token to play this song'
            : 'Failed to start session: $msg';
        state = PlayerState.stopped;
        notifyListeners();
        return;
      }

      currentSong    = song;
      currentSession = session;

      // librats path: rats_send_binary chunks are reassembled in a temp file,
      // then media_kit plays from disk. HTTP fallback returns the streaming
      // URL directly — only useful for nodes that aren't behind NAT.
      final mediaUri = await _client.fetchAudioToCache(song.contentHash);
      // `open` defaults to `play: true`, but pairing that with the
      // explicit `play()` below races libmpv on Android: the second play
      // call can land while the first is mid-startup, and mpv ends up
      // paused at offset 0 until the user scrubs the position. Opening
      // with `play: false` and then calling `play()` ourselves removes
      // the ambiguity — there's exactly one start signal.
      await _player.open(Media(mediaUri.toString()), play: false);
      await _player.play();

      // isPlaying gates each tick so heartbeats only fire while the user
      // is actually listening — pause / stop / load freezes the session's
      // position_ms from the server's perspective, which keeps the
      // union-of-timestamp-ranges play check from synthesizing listen
      // credit for a parked song.
      _heartbeat.start(
        session.sessionId,
        () => positionMs,
        isPlaying:      () => state == PlayerState.playing,
        contentHash:    song.contentHash,
        blockHash:      session.blockHash,
        playerAddress:  playerAddress,
        songDurationMs: song.durationMs,
      );

      state = PlayerState.playing;
      notifyListeners();
    } catch (e) {
      errorMessage = e.toString();
      state        = PlayerState.stopped;
      notifyListeners();
    }
  }

  void _onComplete() {
    _heartbeat.stop();
    // Tell the full node the session is finished so it can run the
    // 50% effective-listen check and mint outputs (escrow + node +
    // discoverer credits). Previously _onComplete only stopped the
    // local heartbeat timer — the session sat in the full node's
    // active-sessions map forever and no MintTx ever fired, which is
    // why a finished song never showed up in play_count even after
    // multiple full listens.
    final finishedId = currentSession?.sessionId;
    if (finishedId != null) {
      unawaited(_completeSessionSilently(finishedId));
    }
    currentSession = null;
    // Advance only when there's a real next track. `playlist.length > 1` is
    // the wrong gate: on a 5-track queue it stays true when the last track
    // finishes, so playNext() wraps via `% playlist.length` back to index 0
    // and the player loops the whole album forever instead of stopping.
    if (_playlistIdx < playlist.length - 1) {
      playNext();
    } else {
      state      = PlayerState.stopped;
      positionMs = 0;
      notifyListeners();
    }
  }
}
