// Global download queue. Owns concurrent fetches from the swarm, exposes
// per-track progress for the UI banner, and absorbs the timeout pain
// that single-album loops used to hit — a stalled track no longer takes
// the rest of the album down with it because each download is its own
// future against a chunk-stall-guarded swarm read.

import 'dart:async';
import 'dart:collection';

import 'package:flutter/foundation.dart';

import '../models/song.dart';
import '../services/library_service.dart';
import '../services/node_client.dart';
import '../services/node_service.dart';
import '../services/rats_client.dart';

enum DownloadStatus {
  queued,
  running,
  done,
  failed,
}

class DownloadJob {
  DownloadJob({
    required this.id,
    required this.song,
    this.variant,
    this.batchLabel,
  });

  /// Stable id used to find this job in the active map. Build from the
  /// canonical content hash + variant peer so retrying the same song with
  /// a different swarm member can coexist.
  final String        id;
  final Song          song;
  final SwarmVariant? variant;

  /// Label that groups jobs ("Download album: <name>", "Download song:
  /// <title>"). Surfaced in the banner so the user sees a single line
  /// per batch instead of a wall of per-track entries.
  final String? batchLabel;

  DownloadStatus status   = DownloadStatus.queued;
  int            received = 0;
  int            total    = 0;
  String?        error;
}

class DownloadProvider extends ChangeNotifier {
  DownloadProvider._();
  static final DownloadProvider instance = DownloadProvider._();

  /// Hard ceiling on parallel TRACK downloads. Swarm Transfer v2: ONE track at a
  /// time. Each track's pieces already fan across EVERY seeder in the swarm
  /// (multi-source), so a single track gets the whole swarm's bandwidth and
  /// finishes ASAP — track 1 is playable while the rest of the album downloads
  /// (the queue is FIFO/album-order). Running tracks in parallel instead just
  /// split the swarm and opened multiple flows per seeder (breaking the
  /// per-seeder 4 Mbit/s window), with no track completing quickly.
  static const int _maxConcurrent = 1;

  final Queue<DownloadJob> _queue   = Queue();
  final Map<String, DownloadJob> _active = {};
  final List<DownloadJob>  _recent  = [];

  /// Active downloads in queue / running state, oldest first. The UI
  /// banner iterates this list.
  List<DownloadJob> get activeJobs =>
      List.unmodifiable([..._active.values, ..._queue]);

  /// Most-recently-completed (or failed) jobs, capped to 5 for the
  /// "recent" trailer in the banner. Cleared by `clearRecent`.
  List<DownloadJob> get recentJobs => List.unmodifiable(_recent);

  /// True while at least one job is queued or running.
  bool get isBusy => _queue.isNotEmpty || _active.isNotEmpty;

  /// 0–1 progress aggregated across active jobs. Indeterminate while we
  /// have a running job whose `total` is still 0 — banner UI should show
  /// the bar as indeterminate in that case.
  double? get aggregateProgress {
    if (_active.isEmpty && _queue.isEmpty) return null;
    var sumDone = 0, sumTotal = 0;
    var anyIndeterminate = false;
    for (final j in [..._active.values, ..._queue]) {
      if (j.total <= 0) {
        anyIndeterminate = true;
      } else {
        sumDone  += j.received;
        sumTotal += j.total;
      }
    }
    if (anyIndeterminate || sumTotal == 0) return null;
    return sumDone / sumTotal;
  }

  // -- Public enqueue helpers --------------------------------------------

  /// Queue a single song. Returns immediately; observe progress via
  /// listenable change notifications. Idempotent — if the same
  /// content_hash is already running or queued, the existing job is
  /// returned and no duplicate is created.
  ///
  /// Short-circuits when the song is already in the local library:
  /// the caller's intent was "I want to have this file" and we already
  /// do, so the job goes straight to `done` and lands on the recent
  /// trailer without ever touching the network. This stops the album
  /// download UI from spinning forever on tracks we already own.
  DownloadJob enqueueSong(Song song,
                          {SwarmVariant? variant, String? batchLabel}) {
    final id = '${song.contentHash}:${variant?.peerId ?? ""}';
    final existing = _findActive(id);
    if (existing != null) return existing;

    final lib    = LibraryService.instance;
    final local  = lib.entryByHash(song.contentHash);
    if (local != null && local.isLocal) {
      final done = DownloadJob(
        id: '$id:local', song: song, variant: variant,
        batchLabel: batchLabel);
      done.status   = DownloadStatus.done;
      done.received = 1;
      done.total    = 1;
      _recent.insert(0, done);
      while (_recent.length > 5) {
        _recent.removeLast();
      }
      notifyListeners();
      return done;
    }

    final job = DownloadJob(
      id: id, song: song, variant: variant, batchLabel: batchLabel);
    _queue.add(job);
    notifyListeners();
    _pump();
    return job;
  }

  /// Queue every track in [songs] under a shared batch label so the UI
  /// banner shows "Downloading album X — 3/12" instead of per-track
  /// noise. Tracks already in the library short-circuit (the underlying
  /// downloadToLibrary call detects them and skips fast).
  void enqueueAlbum(String albumName, List<Song> songs) {
    final label = 'Album: $albumName';
    for (final s in songs) {
      enqueueSong(s, batchLabel: label);
    }
  }

  /// Drop entries from the "recent" trailer once the user has
  /// acknowledged them by closing the banner or starting a new batch.
  void clearRecent() {
    if (_recent.isEmpty) return;
    _recent.clear();
    notifyListeners();
  }

  // -- Internals ---------------------------------------------------------

  DownloadJob? _findActive(String id) {
    final inFlight = _active[id];
    if (inFlight != null) return inFlight;
    for (final q in _queue) {
      if (q.id == id) return q;
    }
    return null;
  }

  void _pump() {
    while (_active.length < _maxConcurrent && _queue.isNotEmpty) {
      final job = _queue.removeFirst();
      _active[job.id] = job;
      job.status = DownloadStatus.running;
      notifyListeners();
      // Fire-and-forget — the future captures `job` so progress
      // callbacks land on the right entry. Errors are caught and
      // recorded in `job.error`.
      unawaited(_run(job));
    }
  }

  /// Hard ceiling per download. The internal `downloadFromSwarm`
  /// already has a chunk-stall guard (~30 s of no bytes → fail and
  /// fall through to the next swarm member), but if every swarm
  /// member is broken in a different way we don't want a single song
  /// to wedge the queue forever. 3 min is generous for a typical 3–10
  /// MB audio file even on a slow link; anything past it is the user
  /// noticing nothing is happening.
  static const _kJobHardCeiling = Duration(minutes: 3);

  Future<void> _run(DownloadJob job) async {
    try {
      final pid = await NodeService.getRatsPeerId(
          waitFor: const Duration(seconds: 8));
      if (pid.isEmpty) {
        throw StateError('No full node discovered yet.');
      }
      final client = NodeClient(ratsPeerId: pid);
      await client.downloadToLibrary(
        job.song.contentHash,
        variant:   job.variant,
        chainSong: job.song,
        onProgress: (received, total) {
          // Progress callbacks can fire at ~kHz on local TCP. Coalesce
          // by only notifying when the % changed by >=0.5 % or the
          // total just became known.
          final oldPct = job.total > 0
              ? (job.received * 200) ~/ job.total
              : -1;
          job.received = received;
          job.total    = total;
          final newPct = job.total > 0
              ? (job.received * 200) ~/ job.total
              : -1;
          if (newPct != oldPct) notifyListeners();
        },
      ).timeout(_kJobHardCeiling, onTimeout: () {
        throw TimeoutException(
            'download exceeded ${_kJobHardCeiling.inMinutes} min — '
            'swarm member unresponsive');
      });
      job.status = DownloadStatus.done;
    } catch (e) {
      job.status = DownloadStatus.failed;
      job.error  = e.toString();
    } finally {
      _active.remove(job.id);
      _recent.insert(0, job);
      while (_recent.length > 5) {
        _recent.removeLast();
      }
      notifyListeners();
      _pump();
    }
  }
}
