// LibraryScanner walks every folder the user added (LibraryService.folders),
// fingerprints any audio file we haven't seen yet, and submits each
// fingerprint to the full node via `fingerprint.submit`. The full node
// either matches (joining us to the existing swarm) or queues the song
// for the next block as a fresh registration.
//
// "Background" today means "periodically triggered while the app is up"
// — flutter_background_service can drive this when the app goes to the
// background, but the same .scanOnce() entry point services both.

import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:audio_metadata_reader/audio_metadata_reader.dart';
import 'package:crypto/crypto.dart' as crypto;
import 'package:permission_handler/permission_handler.dart';

import 'fingerprinter.dart';
import 'library_service.dart';
import 'node_service.dart';
import 'rats_client.dart';

/// Best-effort prompt for the storage permissions LibraryScanner needs.
/// On Android 13+ READ_MEDIA_AUDIO covers Music/ and Podcasts/, but
/// user-picked folders outside those trees require MANAGE_EXTERNAL_STORAGE
/// (the "All files access" toggle), which has its own settings-page flow.
/// Exposed via `LibraryScanner.ensureStoragePermissions` so the folder-picker
/// UI can prompt before the system picker opens.
Future<void> _ensureStoragePermissions() async {
  if (!Platform.isAndroid) return;
  try {
    // READ_MEDIA_AUDIO is a normal runtime permission — request inline.
    final audio = await Permission.audio.status;
    if (audio.isDenied) await Permission.audio.request();
  } catch (_) {/* old plugin/older Android — fall through */}
  try {
    final mgr = await Permission.manageExternalStorage.status;
    if (!mgr.isGranted) await Permission.manageExternalStorage.request();
  } catch (_) {/* not available — user can still scan Music/ via audio */}
}

/// Every container the player will try to fingerprint + serve. media_kit
/// (libmpv) plays each of these natively; the Android-side fingerprint
/// JNI uses MediaCodec which covers most of them too — formats that fail
/// to decode just count as scan errors and skip, they don't break the
/// rest of the library.
const _audioExtensions = {
  '.mp3', '.ogg', '.flac', '.m4a', '.aac', '.opus',
  '.wav', '.wma', '.aif', '.aiff', '.ape', '.mka', '.oga',
};

/// File extension → chain-side `audio_format` tag string. The chain
/// enum lives in [src/core/block.h]; this map MUST stay in lockstep
/// with its `audio_format_from_string` helper or swarm metadata will
/// silently coerce to OGG on round-trip.
String _formatFromPath(String path) {
  final lower = path.toLowerCase();
  if (lower.endsWith('.mp3'))  return 'mp3';
  if (lower.endsWith('.flac')) return 'flac';
  if (lower.endsWith('.m4a'))  return 'm4a';
  if (lower.endsWith('.aac'))  return 'aac';
  if (lower.endsWith('.opus')) return 'opus';
  if (lower.endsWith('.wav'))  return 'wav';
  if (lower.endsWith('.aif') || lower.endsWith('.aiff')) return 'aiff';
  if (lower.endsWith('.wma'))  return 'wma';
  if (lower.endsWith('.ape'))  return 'ape';
  if (lower.endsWith('.mka'))  return 'mka';
  // .ogg / .oga and anything else fall back to ogg — the chain decoder
  // is tolerant on unknown tags and the file extension still carries
  // the real format through to libmpv.
  return 'ogg';
}

class LibraryScanner {
  LibraryScanner._();
  static final LibraryScanner instance = LibraryScanner._();

  /// Prompt for the storage permissions LibraryScanner needs. Safe to call
  /// from UI before opening the system folder picker so the user grants
  /// "All files access" once instead of finding the picker empty.
  Future<void> ensureStoragePermissions() => _ensureStoragePermissions();

  /// Tell the full node we no longer have the bytes for each
  /// [contentHashes]. Uses the new `swarm.remove` delta verb (one
  /// round-trip for the whole batch instead of N).
  Future<void> deannounce(List<String> contentHashes) async {
    if (contentHashes.isEmpty) return;
    final rats = RatsClient.instance;
    final homePid = await NodeService.getRatsPeerId(
        waitFor: const Duration(seconds: 4));
    if (homePid.isEmpty) return;
    try {
      await rats.request(homePid, 'swarm.remove', {
        'peer_id': rats.ownPeerId,
        'hashes':  contentHashes,
      }, timeout: const Duration(seconds: 6));
    } catch (_) {/* best effort */}
  }

  /// Bring the full node's swarm picture of us back in sync with the
  /// local library. Cheap when nothing changed: one digest preflight
  /// fits in a single ~96-byte round-trip and the full node refreshes
  /// our TTL without us listing a single hash. Falls through to a full
  /// `swarm.hello` only when the digest mismatches.
  ///
  /// Used by both the boot-time announce and the VPS-reconnect handler.
  /// Replaces the old per-track `fingerprint.submit` loop that flooded
  /// the network at every reconnect with one RPC per song.
  Future<void> reAnnounce() => syncSwarm();

  /// Compute the local canonical-hash set, send `swarm.hello_digest`,
  /// and if the full node has us under a different set fall through to
  /// `swarm.hello` with the full list. Returns silently on no-op.
  Future<void> syncSwarm() async {
    final lib = LibraryService.instance;
    await lib.ensureLoaded();
    if (lib.entries.isEmpty) return;
    final rats = RatsClient.instance;
    final homePid = await NodeService.getRatsPeerId(
        waitFor: const Duration(seconds: 8));
    if (homePid.isEmpty) return;

    final sorted = _localHashSet(lib);
    if (sorted.isEmpty) return;
    final digest = _hashSetDigest(sorted);

    // Preflight. When the full node already has our set cached with
    // the same digest AND the chain still has at least one of those
    // hashes registered as a song, fast-skip the full swarm.hello +
    // resubmit. If the chain was wiped between sessions (heartbeats
    // only, no song-bearing blocks) the swarm cache can still match
    // because it lives in a separate leveldb prefix — in that case we
    // MUST fall through and re-fire fingerprint.submit so the chain
    // gets the songs back.
    try {
      final reply = await rats.request(homePid, 'swarm.hello_digest', {
        'peer_id': rats.ownPeerId,
        'digest':  digest,
        'count':   sorted.length,
      }, timeout: const Duration(seconds: 6));
      if (reply is Map && reply['match'] == true) {
        // Sanity-probe the chain: does it actually carry any of our
        // local songs? `songs.get` always returns a stub object even
        // for unknown content_hashes (play_count=0, discoverer=zeros),
        // so the right signal is whether `discoverer` is non-zero —
        // that's only set when the song was actually registered. A
        // miss here with a non-empty local library means the chain
        // was wiped between sessions while the server's swarm cache
        // didn't expire; we fall through and re-fire fingerprint.submit.
        bool chainHasOurSongs = false;
        try {
          final probe = await rats.request(homePid, 'songs.get',
              {'content_hash': sorted.first},
              timeout: const Duration(seconds: 5));
          if (probe is Map) {
            final disc = (probe['discoverer'] as String? ?? '')
                .replaceAll('0', '');
            chainHasOurSongs = disc.isNotEmpty;
          }
        } catch (_) { /* treat as missing */ }
        if (chainHasOurSongs) {
          // ignore: avoid_print
          print('[scanner] swarm digest matched (${sorted.length} hashes)'
                ' + chain confirmed — no resync needed');
          return;
        }
        // ignore: avoid_print
        print('[scanner] swarm digest matched BUT chain lost songs — '
              'forcing full swarm.hello + resubmit');
      }
    } catch (_) { /* fall through to full sync */ }

    // Digest miss: send the full sorted list. The full node replaces
    // our swarm membership wholesale and returns any hashes it doesn't
    // recognize so we know to follow up with fingerprint.submit for
    // those (the chain hasn't seen them yet).
    try {
      final reply = await rats.request(homePid, 'swarm.hello', {
        'peer_id': rats.ownPeerId,
        'hashes':  sorted,
      }, timeout: const Duration(seconds: 30));
      if (reply is Map) {
        final unknown = (reply['unknown'] as List?)?.cast<String>()
                       ?? const <String>[];
        // ignore: avoid_print
        print('[scanner] swarm.hello -> known=${reply['peer_size']}'
              ' unknown=${unknown.length}');
        // The chain doesn't know these `unknown` hashes (either fresh
        // chain, or chain was wiped between sessions). Re-fire
        // fingerprint.submit for each one so the songs actually land in
        // the mempool and get minted — swarm.hello alone only registers
        // membership in the swarm tracker, not the chain. We use
        // _processFile(force: true) so the hash-only preflight inside
        // it shortcuts when the chain DOES already have a match, and
        // re-fingerprints + submits the full blob when it doesn't.
        if (unknown.isNotEmpty) {
          // ignore: avoid_print
          print('[scanner] re-submitting ${unknown.length} unknown hashes '
                'to chain via fingerprint.submit');
          final unknownSet = unknown.toSet();
          for (final entry in lib.entries) {
            if (!unknownSet.contains(entry.contentHash) &&
                !unknownSet.contains(entry.canonicalHash)) {
              continue;
            }
            if (entry.filePath.isEmpty) continue;
            final file = File(entry.filePath);
            if (!await file.exists()) continue;
            await _processFile(file, lib, rats, homePid, force: true);
          }
        }
      }
    } catch (_) {/* best effort — next sync will retry */}
  }

  /// Push a single hash add to the full node. Use when LibraryService
  /// has just learned about a freshly-fingerprinted file — saves the
  /// player from waiting until the next syncSwarm to be visible.
  Future<void> announceAddition(LibraryEntry entry) async {
    if (!entry.isLocal) return;
    final hash = entry.canonicalHash.isNotEmpty
        ? entry.canonicalHash
        : entry.contentHash;
    if (hash.isEmpty) return;
    final rats = RatsClient.instance;
    final homePid = await NodeService.getRatsPeerId(
        waitFor: const Duration(seconds: 4));
    if (homePid.isEmpty) return;
    try {
      await rats.request(homePid, 'swarm.add', {
        'peer_id': rats.ownPeerId,
        'hashes':  [hash.toLowerCase()],
      }, timeout: const Duration(seconds: 6));
    } catch (_) {/* best effort — next sync covers it */}
  }

  // ---- Hash-set helpers ------------------------------------------------

  List<String> _localHashSet(LibraryService lib) {
    final seen = <String>{};
    for (final e in lib.entries) {
      if (!e.isLocal) continue;
      final hash = e.canonicalHash.isNotEmpty
          ? e.canonicalHash
          : e.contentHash;
      if (hash.isEmpty) continue;
      seen.add(hash.toLowerCase());
    }
    final out = seen.toList()..sort();
    return out;
  }

  String _hashSetDigest(List<String> sortedHexHashes) {
    final out = BytesBuilder();
    for (final h in sortedHexHashes) {
      final raw = _hexToBytes(h);
      if (raw == null) continue;
      out.add(raw);
    }
    final bytes = out.toBytes();
    if (bytes.isEmpty) return '';
    return crypto.sha256.convert(bytes).toString();
  }

  Uint8List? _hexToBytes(String hex) {
    if (hex.length % 2 != 0) return null;
    final out = Uint8List(hex.length ~/ 2);
    for (int i = 0; i < out.length; ++i) {
      final v = int.tryParse(hex.substring(i * 2, i * 2 + 2), radix: 16);
      if (v == null) return null;
      out[i] = v;
    }
    return out;
  }

  bool _running = false;
  int  _scanned   = 0;
  int  _registered = 0;
  int  _matched   = 0;
  int  _errors    = 0;

  bool get isScanning => _running;
  int  get scanned    => _scanned;
  int  get registered => _registered;
  int  get matched    => _matched;
  int  get errors     => _errors;

  /// Walk every configured folder once. Returns immediately if a scan
  /// is already in flight.
  ///
  /// Pass [force] = true to re-process every file even when its row is
  /// already in the library. The user-triggered "Scan now" button wires
  /// this — without it, a wiped chain (or a swarm that lost track of us)
  /// stays broken because the fast path skips every file we've seen.
  /// With it on, every file gets re-fingerprinted and re-submitted, so
  /// the full node has a chance to re-register or re-add us to the
  /// swarm.
  Future<void> scanOnce({
    void Function()? onProgress,
    bool             force = false,
  }) async {
    if (_running) return;
    _running = true;
    _scanned = 0;
    _registered = 0;
    _matched = 0;
    _errors = 0;
    onProgress?.call();

    // Without storage permissions File(...).list() returns empty on
    // Android 13+. Prompt before the walk; if the user declines, the
    // scan still runs and reports zero files instead of silently failing.
    await _ensureStoragePermissions();

    try {
      final lib = LibraryService.instance;
      await lib.ensureLoaded();
      final rats = RatsClient.instance;
      final homePid = await NodeService.getRatsPeerId(
          waitFor: const Duration(seconds: 8));
      if (homePid.isEmpty) {
        _errors += 1;
        return;
      }

      // Process files in batches of 2 so one is decoding on the Kotlin
      // pool while the previous batch's RPC is in flight on the VPS
      // relay. With sequential awaits the loop hit a hard floor of
      // (decode + RPC) per file; this overlaps them and roughly halves
      // the wall-clock for an album scan. concurrency=2 matches the
      // Kotlin-side decode executor width.
      const concurrency = 2;
      final batch = <File>[];
      Future<void> drain() async {
        if (batch.isEmpty) return;
        await Future.wait(batch.map((f) async {
          await _processFile(f, lib, rats, homePid, force: force);
          _scanned += 1;
          onProgress?.call();
        }));
        batch.clear();
      }

      for (final folder in lib.folders) {
        final dir = Directory(folder);
        if (!await dir.exists()) continue;
        await for (final entity in dir.list(recursive: true,
                                            followLinks: false)) {
          if (entity is! File) continue;
          final lower = entity.path.toLowerCase();
          if (!_audioExtensions.any((ext) => lower.endsWith(ext))) continue;
          batch.add(entity);
          if (batch.length >= concurrency) await drain();
        }
      }
      await drain();
      // After every file has been (re-)submitted individually, push
      // the consolidated digest to the full node so future boots can
      // skip the per-track work entirely via swarm.hello_digest.
      unawaited(syncSwarm());
    } finally {
      _running = false;
      onProgress?.call();
    }
  }

  Future<void> _processFile(File file, LibraryService lib,
                            RatsClient rats, String homePid,
                            {bool force = false}) async {
    try {
      // O(1) path lookup. The old loop here was O(N) per file × N files
      // = quadratic; a 500-track library re-scan took minutes purely
      // from this loop before any actual work happened.
      final LibraryEntry? existingByPath = lib.entryByPath(file.path);

      // Fast-path skip: known file + not forcing → nothing to do.
      if (!force && existingByPath != null) {
        return;
      }

      // Force-path quick check (no file read, no decode): ping the
      // full node with the cached fingerprint_hash. If it matches,
      // we're already swarm-joined and done.
      if (force && existingByPath != null
          && existingByPath.fingerprintHash.isNotEmpty) {
        try {
          final reply = await rats.request(homePid, 'fingerprint.submit', {
            'fingerprint_hash': existingByPath.fingerprintHash,
            'peer_id':          rats.ownPeerId,
            'content_hash':     existingByPath.contentHash,
            'title':            existingByPath.title,
            'artist':           existingByPath.artist,
            'genre':            existingByPath.genre,
            'album':            existingByPath.album,
            'year':             existingByPath.year,
            'track_number':     existingByPath.trackNumber,
            'bitrate':          existingByPath.bitrate,
            'duration_ms':      existingByPath.durationMs,
            'audio_format':     existingByPath.audioFormat,
          }, timeout: const Duration(seconds: 10));
          final m = (reply is Map<String, dynamic>) ? reply : const {};
          if (m['matched'] == true) {
            _matched += 1;
            return; // full node has it, swarm join confirmed — done
          }
          // matched=false → song was never on chain or chain was
          // wiped. Fall through to the full re-fingerprint path.
        } catch (_) {
          // Submit failed (timeout, etc); fall through to full process.
        }
      }

      // Fall-through path: read bytes, compute content_hash, run full
      // fingerprint + tag extraction + submit. ONLY runs for new files
      // or force-rescans where the hash-only probe missed.
      final bytes = await file.readAsBytes();
      final contentHash =
          crypto.sha256.convert(bytes).toString();

      // Re-check by hash after the read — covers the case where the
      // file got copied/moved to a new path under the same library.
      final existing = lib.entryByHash(contentHash);
      if (!force && existing != null && existing.filePath == file.path) {
        return;
      }

      // Compute the chromaprint fingerprint locally.
      final fp = await Fingerprinter.ofFile(file.path);

      // Read container tags (ID3 v1/v2, MP4, FLAC, Vorbis comments).
      // Falls back to filename heuristics if the container has nothing
      // useful — better than the empty strings we shipped before.
      final tag = _readTagsBestEffort(file);

      final fname  = file.uri.pathSegments.last;
      final dotIdx = fname.lastIndexOf('.');
      final stem   = dotIdx > 0 ? fname.substring(0, dotIdx) : fname;
      final fileTitle = stem.replaceAll('_', ' ').trim();

      final title  = _pick(tag?.title, fileTitle);
      final artist = _pick(tag?.artist, '');
      final album  = _pick(tag?.album,  '');
      final genre  = (tag?.genres.isNotEmpty == true)
                       ? tag!.genres.first
                       : '';
      // ID3 numerics: 0 means "tag didn't carry it." The full node
      // treats 0 as "unknown" and the UI hides the chip when it's 0.
      final year         = tag?.year?.year ?? 0;
      final trackNumber  = tag?.trackNumber ?? 0;
      // Container reports bitrate in bits/sec. The full node stores it
      // with the SwarmMember so the download dialog can let the user
      // pick a quality and streaming defaults to the lowest.
      final bitrate      = tag?.bitrate ?? 0;

      // Duration: prefer the container's own value (much more accurate
      // than counting PCM samples, which doubled stereo files because we
      // weren't dividing by channelCount). Fall back to PCM math.
      final tagDurMs = tag?.duration?.inMilliseconds ?? 0;
      final pcmDurMs = (fp.pcmSamples /
                       (fp.channelCount > 0 ? fp.channelCount : 1) /
                       fp.sampleRate * 1000).round();
      final durationMs = tagDurMs > 0 ? tagDurMs : pcmDurMs;

      final fmt = _formatFromPath(file.path);

      // Submit to the full node.
      final reply = await rats.request(homePid, 'fingerprint.submit', {
        'fingerprint':      fp.compressed,
        'fingerprint_hash': fp.fingerprintHash,
        'peer_id':          rats.ownPeerId,
        'content_hash':     contentHash,
        'title':            title,
        'artist':           artist,
        'genre':            genre,
        'album':            album,
        'duration_ms':      durationMs,
        'year':             year,
        'track_number':     trackNumber,
        'bitrate':          bitrate,
        'audio_format':     fmt,
      }, timeout: const Duration(seconds: 20));

      final m = (reply as Map<String, dynamic>?) ?? const {};
      final matched    = m['matched']    == true;
      final registered = m['registered'] == true;
      if (matched) _matched += 1;
      if (registered) _registered += 1;

      await lib.upsert(LibraryEntry(
        contentHash:     contentHash,
        fingerprintHash: fp.fingerprintHash,
        title:           title,
        artist:          artist,
        album:           album,
        genre:           genre,
        year:            year,
        trackNumber:     trackNumber,
        bitrate:         bitrate,
        durationMs:      durationMs,
        audioFormat:     fmt,
        filePath:        file.path,
        addedAtMs:       DateTime.now().millisecondsSinceEpoch,
      ));
    } catch (_) {
      _errors += 1;
    }
  }

  /// Try every parser in the package; swallow exceptions because a
  /// missing or malformed tag block is not a scan failure.
  AudioMetadata? _readTagsBestEffort(File f) {
    try { return readMetadata(f, getImage: false); } catch (_) { return null; }
  }

  String _pick(String? a, String fallback) =>
      (a != null && a.trim().isNotEmpty) ? a.trim() : fallback;
}

