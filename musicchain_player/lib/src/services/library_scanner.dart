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

import 'package:audio_metadata_reader/audio_metadata_reader.dart';
import 'package:crypto/crypto.dart' as crypto;
import 'package:permission_handler/permission_handler.dart';

import 'fingerprinter.dart';
import 'library_publisher.dart';
import 'presence_publisher.dart';
import 'playlist_service.dart';
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

  // EIP-55 0x-prefixed 42-char wallet address the user currently has
  // loaded. WalletProvider sets this whenever a wallet is created /
  // imported / auto-loaded so the next fingerprint.submit attaches it
  // as the artist_address — the chain then credits future play
  // royalties to this wallet instead of the zero address (which is
  // what happened when the field was omitted). The chain's
  // parse_address_checksummed accepts the full 0x form directly.
  static String _artistAddress = '';
  // ignore: avoid_setters_without_getters
  static set artistAddress(String addr) {
    _artistAddress = addr;
  }

  /// Prompt for the storage permissions LibraryScanner needs. Safe to call
  /// from UI before opening the system folder picker so the user grants
  /// "All files access" once instead of finding the picker empty.
  Future<void> ensureStoragePermissions() => _ensureStoragePermissions();

  /// Tell the mesh we no longer have the bytes for each [contentHashes].
  /// Removal now flows through DB2: LibraryService has already dropped the
  /// rows, so a fresh `library.delta` snapshot (which lists exactly what we
  /// still hold) carries the removal to the full node and, via flood, every
  /// other node. The [contentHashes] arg is retained for call-site
  /// compatibility but the snapshot is what's authoritative.
  Future<void> deannounce(List<String> contentHashes) async {
    if (contentHashes.isEmpty) return;
    unawaited(LibraryPublisher.publishFull());
  }

  /// Re-establish our standing with the full node on (re)connect. Declares the
  /// wallet<->live-peer binding via signed `presence.hello`, then republishes
  /// the wallet-signed library (DB2 / `library.delta`) and saved playlists.
  ///
  /// Used by both the boot-time announce and the VPS-reconnect handler. The old
  /// `swarm.hello`/digest membership round-trips are gone — the durable library
  /// now lives in DB2 and floods to the rest of the mesh from the home node.
  Future<void> reAnnounce() async {
    // Presence — declare the wallet<->live-peer binding for this connection.
    // Replaces the old swarm.hello library re-announce; the durable library now
    // lives in DB2 (library.delta) and is republished just below.
    await PresencePublisher.announce();
    // DB2 — also publish the wallet-signed library to the gossip-replicated
    // LibraryStore so the full node (and, via flood, every other node) has our
    // current list. Fire-and-forget; the node's version gate makes it idempotent.
    unawaited(LibraryPublisher.publishFull());
    // Same for saved playlists — re-publish on reconnect so edits made while
    // the home node was unreachable converge. Idempotent (version-gated).
    unawaited(PlaylistService.instance.republishAll());
  }

  /// Re-fire `fingerprint.submit` for every local file whose content hash the
  /// chain doesn't yet know. Called by [LibraryPublisher.publishFull] with the
  /// `unknown[]` list the node returns on a `library.delta` reply (the hashes
  /// in our published library that aren't registered on chain — either a fresh
  /// chain or one wiped between sessions).
  ///
  /// This is the SAME resubmit the old `swarm.hello` reply path performed: it
  /// maps each unknown hash back to a local file (by `contentHash` or
  /// `canonicalHash`) and runs `_processFile(force: true)`, whose hash-only
  /// preflight shortcuts when the chain DOES already have a match and
  /// re-fingerprints + submits the full blob when it doesn't.
  Future<void> resubmitUnknown(List<String> hashes) async {
    if (hashes.isEmpty) return;
    final lib = LibraryService.instance;
    await lib.ensureLoaded();
    if (lib.entries.isEmpty) return;
    final rats = RatsClient.instance;
    final homePid = await NodeService.getRatsPeerId(
        waitFor: const Duration(seconds: 8));
    if (homePid.isEmpty) return;

    // ignore: avoid_print
    print('[scanner] re-submitting ${hashes.length} unknown hashes '
          'to chain via fingerprint.submit');
    final unknownSet = hashes.toSet();
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

  /// A freshly-fingerprinted file just landed in the library. Republish the
  /// whole library to DB2 so the new content hash lands in the gossip-replicated
  /// LibraryStore (and, via flood, on every node). The publish is a versioned
  /// SNAPSHOT, so it naturally supersedes the previous list. Replaces the old
  /// per-hash `swarm.add`.
  Future<void> announceAddition(LibraryEntry entry) async {
    if (!entry.isLocal) return;
    final hash = entry.canonicalHash.isNotEmpty
        ? entry.canonicalHash
        : entry.contentHash;
    if (hash.isEmpty) return;
    unawaited(LibraryPublisher.publishFull());
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
      // After every file has been (re-)submitted individually, publish the
      // updated library SNAPSHOT to DB2 so the full node (and, via flood, the
      // rest of the mesh) converges on our current list. The node's reply may
      // carry an `unknown[]` of hashes the chain doesn't yet know, which
      // publishFull itself feeds back into resubmitUnknown → _processFile.
      //
      // Awaited under the _running guard: publishFull → resubmitUnknown can
      // invoke _processFile(force: true) for any hashes the chain doesn't
      // recognize, which mutates _scanned/_matched/_registered/_errors and
      // calls lib.upsert. If we fire-and-forget here, the finally clears
      // _running while that resubmit is still in flight; the next scanOnce
      // trigger (periodic background, user tap, VPS reconnect) would pass the
      // `if (_running) return` gate, zero the counters mid-flight, and run
      // _processFile concurrently with the still-running resubmit on
      // overlapping files — double fingerprint.submit RPCs, lost counter
      // updates, and racing lib.upsert writes on the same entry.
      await LibraryPublisher.publishFull();
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
            if (_artistAddress.isNotEmpty) 'artist_address': _artistAddress,
          }, timeout: const Duration(seconds: 10));
          // `reply is Map` (not Map<String,dynamic>) so a relay-decoded
          // Map<dynamic,dynamic> still hits the fast path instead of falling
          // through to a needless full re-fingerprint.
          final m = (reply is Map) ? reply : const {};
          if (m['matched'] == true) {
            // Same corroboration gate as the full-submit path below: only count
            // a match toward the displayed tally when the node names a chain
            // song that actually has a swarm, so a fuzzy false-positive can't
            // inflate "matched N". The early return (swarm-join already done)
            // stays regardless — that's a real fingerprint_hash hit.
            final chainHash = (m['content_hash'] as String?)?.trim() ?? '';
            final swarmSize = (m['swarm_size'] is num)
                ? (m['swarm_size'] as num).toInt() : 0;
            if (chainHash.isNotEmpty && swarmSize > 0) _matched += 1;
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
      // Sanity-gate the duration. A container/tag can report a wildly wrong
      // value (seen registered on-chain: 662646176 ms = 184 h). Because the
      // play-reward gate requires listening to 50% of the REGISTERED duration,
      // a garbage duration makes the song PERMANENTLY un-rewardable (every real
      // play scores "below_threshold"). Accept a value only if it's plausible
      // for a track (< 6 h): prefer the tag, fall back to the PCM-derived
      // duration, else 0 so the node applies its fixed legacy listen threshold.
      const kMaxSaneDurationMs = 6 * 60 * 60 * 1000; // 6 hours
      final durationMs = (tagDurMs > 0 && tagDurMs < kMaxSaneDurationMs)
          ? tagDurMs
          : (pcmDurMs > 0 && pcmDurMs < kMaxSaneDurationMs ? pcmDurMs : 0);

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
        if (_artistAddress.isNotEmpty) 'artist_address': _artistAddress,
      }, timeout: const Duration(seconds: 20));

      // Safe narrowing: `as Map<String, dynamic>?` throws TypeError if
      // the RPC came back as Map<dynamic, dynamic> (the common shape
      // after a VPS relay decode/re-encode round-trip) or anything else
      // non-null, which would jump straight to the outer catch and
      // count this file as an error WITHOUT calling lib.upsert below —
      // even though fingerprint.submit succeeded server-side and the
      // chain already enqueued our registration. We'd then re-read,
      // re-fingerprint, and re-submit the same file on every future
      // scan cycle. Use an `is`-check (same pattern as the force-path
      // probe earlier in this function) so an unparseable body just
      // means "no matched/registered flag" and we still record the
      // entry locally.
      final m = (reply is Map) ? reply : const {};
      final matched    = m['matched']    == true;
      final registered = m['registered'] == true;
      // Don't trust a bare matched/registered flag for the displayed tally.
      // The node returns content_hash + swarm_size on a real match; only count
      // a match when it names a chain song that actually has a swarm. This
      // stops a node-side fuzzy false-positive (matched:true with no real
      // swarm) from inflating the "matched N" progress counter into songs that
      // aren't there. (The library list itself is built from local files +
      // their own tags, so it was never mislabeled — this is the counter fix.)
      final chainHash = (m['content_hash'] as String?)?.trim() ?? '';
      final swarmSize = (m['swarm_size'] is num)
          ? (m['swarm_size'] as num).toInt() : 0;
      if (matched && chainHash.isNotEmpty && swarmSize > 0) _matched += 1;
      if (registered) _registered += 1;

      await lib.upsert(LibraryEntry(
        contentHash:     contentHash,
        // On a genuine fuzzy match the chain song's canonical hash differs from
        // our local byte hash; record it so variant-linking / the chain-library
        // "local" badge resolve this file to the existing song. Title/artist
        // stay LOCAL — we never adopt chain metadata.
        canonicalHash:   (chainHash.isNotEmpty && chainHash != contentHash)
                             ? chainHash : '',
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

