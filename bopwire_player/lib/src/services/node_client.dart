import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart' as crypto;
import 'package:path_provider/path_provider.dart';

import '../models/song.dart';
import '../models/session.dart';
import 'audio_stream_proxy.dart';
import 'device_fingerprint_service.dart';
import 'library_service.dart';
import 'piece_downloader.dart';
import 'piece_manifest.dart';
import 'rats_client.dart';
import 'swarm_registry.dart';

/// Thin shim over [RatsClient] that exposes every full-node verb the rest
/// of the player uses. All traffic goes over librats TCP — direct to the
/// full node when its NAT lets us, or wrapped in `relay.forward` through
/// the VPS mini-node otherwise (configured by [LibratsDiscovery]).
///
/// The old HTTP/1.1 fallback was removed once libmicrohttpd and the
/// standalone HTTP/3 server were deleted from the full node. If/when an
/// msh3 listener comes back, add it here as a separate method rather than
/// resurrecting `http.get`.
class NodeClient {
  String? ratsPeerId;

  NodeClient({this.ratsPeerId});

  bool get _ratsReady =>
      ratsPeerId != null && ratsPeerId!.isNotEmpty && _rats != null;

  /// Cached RatsClient handle so we don't hit RatsClient.instance's
  /// throw-and-catch path on every RPC. The static singleton becomes
  /// available once at startup and never goes away during the process
  /// lifetime, so caching the resolved instance is safe and dodges
  /// the per-call try/catch cost.
  static RatsClient? _cachedRats;
  RatsClient? get _rats {
    final c = _cachedRats;
    if (c != null) return c;
    try {
      final fresh = RatsClient.instance;
      _cachedRats = fresh;
      return fresh;
    } catch (_) {
      return null;
    }
  }

  Future<Object?> _rpc(String type, Map<String, dynamic> body,
      {Duration timeout = const Duration(seconds: 15)}) {
    if (!_ratsReady) {
      throw StateError('node_client: no rats peer configured for $type');
    }
    return _rats!.request(ratsPeerId!, type, body, timeout: timeout);
  }

  // ---- Status ---------------------------------------------------------

  Future<Map<String, dynamic>> getStatus() async {
    final r = await _rpc('status', const {});
    return Map<String, dynamic>.from(r as Map);
  }

  // ---- Songs ----------------------------------------------------------

  List<Song> _decodeSongs(Object? r) {
    final list = (r as List).cast<dynamic>();
    return list.map((e) => Song.fromJson(e as Map<String, dynamic>)).toList();
  }

  Future<List<Song>> getSongs({int page = 1}) async =>
      _decodeSongs(await _rpc('songs.list', const {}));

  Future<Song> getSong(String contentHash) async {
    final r = await _rpc('songs.get', {'content_hash': contentHash});
    return Song.fromJson(Map<String, dynamic>.from(r as Map));
  }

  /// True when the node already has a song with [contentHash] on chain.
  /// Used by the folder uploader to skip already-uploaded files.
  ///
  /// We distinguish "not found" (a clean RPC reply telling us the song
  /// isn't on chain) from transport failures (timeout, send_failed,
  /// server_error) — the latter rethrows so the upload-skip logic
  /// doesn't silently treat a flaky network as "this song is new" and
  /// duplicate-upload everything.
  Future<bool> hasSong(String contentHash) async {
    if (!_ratsReady) return false;
    try {
      await getSong(contentHash);
      return true;
    } on RatsRpcException catch (e) {
      // Treat only error-style replies as "not present"; transport
      // failures bubble. wrap_handler_result on the full node maps any
      // non-2xx HTTP status into `http_<code>`, so a server-side blowup
      // arrives here as `http_5xx` — not `server_error`. Bubble those
      // too, otherwise a transient 500/502/503 looks indistinguishable
      // from "song isn't on chain" and the upload-skip logic dupes the
      // file across every retry.
      if (e.status == 'timeout' || e.status == 'send_failed' ||
          e.status == 'server_error' ||
          e.status.startsWith('http_5')) {
        rethrow;
      }
      return false;
    } catch (_) {
      // Type-cast failures from Song.fromJson on malformed reply are
      // safe to treat as "missing" since we'd reupload to fix it.
      return false;
    }
  }

  Future<List<Song>> searchSongs(String query) async =>
      _decodeSongs(await _rpc('songs.search', {'q': query}));

  Future<List<Song>> searchSongsByArtist(String artist) async =>
      _decodeSongs(await _rpc('songs.search', {'artist': artist}));

  Future<List<Song>> searchSongsByGenre(String genre) async =>
      _decodeSongs(await _rpc('songs.search', {'genre': genre}));

  // ---- DMCA inbox -----------------------------------------------------

  /// Upload a takedown PDF for the full node moderator to review. The
  /// node sanitizes [filename], drops it in `<data_dir>/dmca/`, and the
  /// moderator's TUI lists it on the F1 page. Returns the
  /// timestamp-prefixed name the node ended up with.
  Future<String> submitDmcaPdf(String filename, Uint8List bytes) async {
    final r = await _rpc(
      'dmca.submit_pdf',
      {
        'filename':    filename,
        'content_b64': base64Encode(bytes),
      },
      // PDFs from law firms can be a few MB on cellular; give the round-
      // trip enough headroom that a slow encode upload still completes.
      timeout: const Duration(minutes: 2),
    );
    final m = Map<String, dynamic>.from(r as Map);
    return (m['stored_as'] as String?) ?? filename;
  }

  /// Push a KYC form / ID scan to the full node so the moderator can
  /// match it to [fromAddress] when releasing that wallet's escrow.
  /// PDF, JPG and PNG are all accepted; node-side cap is 32 MB.
  Future<String> submitKycForm({
    required String    filename,
    required Uint8List bytes,
    required String    fromAddress,
  }) async {
    final r = await _rpc(
      'kyc.submit_form',
      {
        'filename':     filename,
        'content_b64':  base64Encode(bytes),
        'from_address': fromAddress,
      },
      timeout: const Duration(minutes: 2),
    );
    final m = Map<String, dynamic>.from(r as Map);
    return (m['stored_as'] as String?) ?? filename;
  }

  // ---- Wallet ---------------------------------------------------------

  Future<int> getWalletNonce(String address) async {
    final r = await _rpc('wallet.nonce', {'address': address});
    // (#crash) coerce defensively — a non-map reply or a JSON double would
    // TypeError and abort the wallet action otherwise.
    final n = r is Map ? (r['nonce'] as num?)?.toInt() : null;
    if (n == null) throw RatsRpcException('bad_reply', 'wallet.nonce missing nonce');
    return n;
  }

  Future<String> submitTransfer({
    required String fromAddress,
    required String toAddress,
    required String amountStr,
    required String signature,
    required String fromPubkey,
    required int nonce,
  }) async {
    final r = await _rpc('wallet.transfer', {
      'from_address': fromAddress,
      'to_address':   toAddress,
      'amount':       amountStr,
      'nonce':        nonce,
      'signature':    signature,
      // Required: the node's verify_signature carries the pubkey inline (no
      // ECDSA recovery) and cross-checks address_from_pubkey == from_address.
      'from_pubkey':  fromPubkey,
    });
    final tx = r is Map ? r['tx_hash'] as String? : null;
    if (tx == null) throw RatsRpcException('bad_reply', 'wallet.transfer missing tx_hash');
    return tx;
  }

  Future<String> getBalance(String address) async {
    final r = await _rpc('wallet.balance', {'address': address});
    // Accept string or numeric balance; never hard-cast.
    final b = r is Map ? r['balance'] : null;
    if (b == null) throw RatsRpcException('bad_reply', 'wallet.balance missing balance');
    return b.toString();
  }

  // ---- Audio streaming ------------------------------------------------

  /// Resolve [contentHash] to a playable file:// URI.
  ///
  /// Routing under the post-pivot architecture:
  ///
  ///   1. If our own library already has the file on disk (we
  ///      fingerprinted it locally), play that path directly — no
  ///      network at all.
  ///   2. Otherwise consult the swarm via `RatsClient.downloadFromSwarm`:
  ///        a. ask the full node for the swarm member list
  ///           (`stream.open` → status=swarm + peers=[...]),
  ///        b. resolve each peer's `public_address` via
  ///           `player.locate` on the VPS,
  ///        c. try direct `rats_connect` first, fall back to VPS relay,
  ///        d. issue `stream.open` against the chosen swarm peer and
  ///           receive its binary chunks.
  ///   3. Write the resulting bytes to a cache file media_kit can play.
  ///
  /// Throws when no swarm member can serve the song.
  Future<Uri> fetchAudioToCache(String contentHash) async {
    final lib = LibraryService.instance;
    await lib.ensureLoaded();
    final localEntry = lib.entryByHash(contentHash);
    if (localEntry != null && localEntry.isLocal) {
      return Uri.file(localEntry.filePath);
    }

    if (!_ratsReady) {
      throw StateError('fetchAudioToCache: no rats peer configured');
    }
    final rats   = _rats!;
    // RELAY-ANCHOR: the vpsPeerId we hand into streamFromSwarm is used as the
    // relay through which player.locate runs and per-peer relay.forward is
    // routed. It MUST be a mini-node — `validatedPeerIds.first` may be a FULL
    // NODE (or another player) that has no relay.forward handler, so the
    // relayed request bounces back as "no handler for type=relay.forward" and
    // no chunk ever flows. Prefer the load-aware mini-node, then the first
    // identified one; if no mini-node is connected leave it null so we skip the
    // locate/relay setup entirely and request() re-resolves the live relay.
    final vpsPid = rats.bestMiniNodePeerId ?? rats.firstMiniNodePeerId;

    // (M1) Cancel the previously-playing stream BEFORE opening the new one, so
    // its serving peer is told to stop pushing (stream.cancel) and the new
    // stream.open doesn't fight the leaked push on the single relay link — the
    // root of the "play track 1, skip to track 2, it hangs ~30 s" bug.
    AudioStreamProxy.instance.cancelCurrent();

    // Streaming path: open the swarm stream and hand media_kit a loopback
    // HTTP URL right away. libmpv starts pulling bytes the moment the
    // proxy is ready and starts decoding as the first KB lands instead
    // of waiting for the whole file to finish downloading first. On
    // cellular this is the difference between ~10 s wait and ~300 ms.
    try {
      final stream = await rats.streamFromSwarm(
        nodePeerId:  ratsPeerId!,
        contentHash: contentHash,
        vpsPeerId:   vpsPid,
      );
      await AudioStreamProxy.instance.ensureStarted();
      final url = AudioStreamProxy.instance.serve(stream);
      // ignore: avoid_print
      print('[stream-diag] PUSH ok: ${stream.totalBytes}B via proxy');
      return Uri.parse(url);
    } on RatsRpcException catch (e) {
      // ignore: avoid_print
      print('[stream-diag] streamFromSwarm FAILED: $e');  // full msg incl. per-peer lastError
      // (DHT un-nerf P1) Streaming discovers sources ONLY via the full
      // node's VPS-mediated swarm. When that's empty/offline the stream
      // throws no_swarm / swarm_exhausted — fall back to the piece
      // downloader, which now discovers AND dials DHT seeders (P0a/P0b), to
      // fetch the bytes to a cache file and play from disk. Slower first
      // byte than streaming, but it's the difference between "plays from
      // another player over the DHT" and "unplayable because the VPS swarm
      // was empty".
      if (e.status == 'no_swarm' || e.status == 'swarm_exhausted') {
        final cached = await _dhtFallbackToCache(contentHash);
        if (cached != null) return cached;
      }
      rethrow;
    }
  }

  /// (DHT un-nerf P1) Fetch [contentHash] via the DHT-enabled piece
  /// downloader to a cache file and return its file Uri, or null if neither
  /// the VPS swarm nor the DHT yields a usable seeder.
  Future<Uri?> _dhtFallbackToCache(String contentHash) async {
    try {
      final dir    = await _downloadsDir();
      final target = File('${dir.path}/$contentHash.audio');
      if (await target.exists() && await target.length() > 0) {
        return Uri.file(target.path);   // already cached from a prior fetch
      }
      final res = await _tryPieceDownload(
        expectedHash:   contentHash,
        canonicalHash:  contentHash,
        finalCachePath: target.path,
      );
      if (res != null && await target.exists() && await target.length() > 0) {
        return Uri.file(target.path);
      }
    } catch (_) { /* fall through — caller rethrows the streaming error */ }
    return null;
  }

  /// Look up every variant (peer × bitrate × format) the full node knows
  /// of for [contentHash]. Used by the download dialog to populate a
  /// quality picker. Empty list = no swarm members (song not yet
  /// announced or its lone holder went offline).
  Future<List<SwarmVariant>> lookupSwarmVariants(String contentHash) async {
    if (!_ratsReady) return const <SwarmVariant>[];
    return _rats!.lookupSwarmVariants(
      nodePeerId:  ratsPeerId!,
      contentHash: contentHash,
    );
  }

  /// Pull every byte of [contentHash] from the swarm, persist it to the
  /// on-device downloads folder, copy the chain song's metadata into the
  /// resulting [LibraryEntry], and announce to the swarm so other
  /// players can fetch from us too.
  ///
  /// Pass [variant] to fetch a specific quality (the UI's picker chose
  /// it). Pass [chainSong] to copy title/artist/album/genre/etc into the
  /// local row (downloaded files keep their embedded ID3 tags inside
  /// the bytes themselves, but the chain has authoritative metadata).
  Future<String> downloadToLibrary(
    String contentHash, {
    SwarmVariant? variant,
    Song?         chainSong,
    /// Fires on every chunk so the DownloadProvider can update its
    /// progress meter without polling. `total` is 0 when the full node
    /// didn't advertise a size (rare; treat as indeterminate).
    void Function(int received, int total)? onProgress,
  }) async {
    final lib = LibraryService.instance;
    await lib.ensureLoaded();
    final existing = lib.entryByHash(contentHash);
    if (existing != null && existing.isLocal) {
      return existing.filePath;
    }

    if (!_ratsReady) {
      throw StateError('downloadToLibrary: no rats peer configured');
    }
    final rats   = _rats!;
    // RELAY-ANCHOR (see fetchAudioToCache): the relay anchor handed to the
    // swarm fetch must be a mini-node, never `validatedPeerIds.first` (which
    // can be a full node with no relay.forward handler). Null when no mini-node
    // is connected — downloadFromSwarm then skips locate/relay and request()
    // re-resolves the relay live on each RPC.
    final vpsPid = rats.bestMiniNodePeerId ?? rats.firstMiniNodePeerId;

    // The bytes we end up with hash to either the canonical contentHash
    // (legacy single-variant chain entry) or to variant.contentHash if
    // the caller picked a specific quality. PieceDownloader needs this
    // value for its integrity check up-front; the legacy fallback path
    // doesn't and recomputes after the bytes land.
    final expectedHash = (variant?.contentHash ?? contentHash).toLowerCase();

    // Pre-compute the file extension so both paths land at the same
    // final location (${dir}/${localHash}.${ext}). The extension order
    // mirrors `audio_format_from_string` in the chain core and
    // `_formatFromPath` in library_scanner.dart — keep those three
    // sites in lockstep when adding a new container.
    final fmt = variant?.audioFormat ??
                (chainSong != null
                    ? (chainSong.contentHash == contentHash
                        ? (existing?.audioFormat ?? 'mp3')
                        : 'mp3')
                    : (existing?.audioFormat ?? 'mp3'));
    final ext = switch (fmt) {
      'ogg'  => 'ogg',
      'flac' => 'flac',
      'm4a'  => 'm4a',
      'aac'  => 'aac',
      'opus' => 'opus',
      'wav'  => 'wav',
      'aiff' => 'aiff',
      'wma'  => 'wma',
      'ape'  => 'ape',
      'mka'  => 'mka',
      _      => 'mp3',
    };
    final dir = await _downloadsDir();

    // Try the multi-peer PieceDownloader path first. It writes directly
    // into the final cache path on success, so on the happy path we
    // skip the in-memory writeAsBytes step entirely (saves a copy on
    // large files and lets the partial bytes survive process restart).
    final pieceTarget = File('${dir.path}/$expectedHash.$ext');
    final piece = await _tryPieceDownload(
      expectedHash:   expectedHash,
      canonicalHash:  contentHash,
      finalCachePath: pieceTarget.path,
      onProgress:     onProgress,
    );

    final Uint8List bytes;
    final String    localHash;
    final File      out;
    if (piece != null) {
      out       = pieceTarget;
      localHash = expectedHash;
      // We won't need the bytes in memory beyond fingerprint.submit,
      // which doesn't use them. Read lazily only if a downstream
      // caller asks; for now leave bytes empty to skip the wasted I/O.
      bytes = Uint8List(0);
    } else {
      final fetched = await rats.downloadFromSwarm(
        nodePeerId:       ratsPeerId!,
        contentHash:      contentHash,
        vpsPeerId:        vpsPid,
        // Internal ceiling per swarm attempt. The DownloadProvider
        // wraps this in a separate 3-min hard cap so a stuck attempt
        // still fails the JOB on schedule even if a single peer's
        // stream produces a slow trickle that never exceeds the
        // chunk-stall threshold.
        totalTimeout:     const Duration(minutes: 2),
        preferredBitrate: variant?.bitrate,
        onProgress:       onProgress,
      );
      bytes     = Uint8List.fromList(fetched);
      localHash = crypto.sha256.convert(bytes).toString();
      out       = File('${dir.path}/$localHash.$ext');
      await out.writeAsBytes(bytes, flush: true);
    }

    // Build the library row from the chain's metadata so the download
    // shows up in My Library with the correct title/artist/album/etc.
    // The variant's bitrate wins over the chain's stored bitrate (the
    // chain only knows the first uploader's bitrate).
    final fpHash = chainSong?.fingerprintHash ?? existing?.fingerprintHash ?? '';
    final updated = LibraryEntry(
      contentHash:     localHash,
      fingerprintHash: fpHash,
      canonicalHash:   contentHash,
      title:           chainSong?.title  ?? existing?.title  ?? '',
      artist:          chainSong?.artist ?? existing?.artist ?? '',
      album:           chainSong?.album  ?? existing?.album  ?? '',
      genre:           chainSong?.genre  ?? existing?.genre  ?? '',
      year:            chainSong?.year   ?? existing?.year   ?? 0,
      trackNumber:     chainSong?.trackNumber ?? existing?.trackNumber ?? 0,
      bitrate:         variant?.bitrate ?? existing?.bitrate ?? 0,
      durationMs:      chainSong?.durationMs ?? existing?.durationMs ?? 0,
      audioFormat:     ext,
      filePath:        out.path,
      addedAtMs:       DateTime.now().millisecondsSinceEpoch,
    );
    await lib.upsert(updated);

    // Announce to the swarm so other players can fetch this variant
    // from us. Exact fingerprint_hash match short-circuits the home
    // node's matching logic, so this is cheap. Best-effort: if the
    // submit fails we still kept the file on disk.
    if (fpHash.isNotEmpty) {
      try {
        await rats.request(ratsPeerId!, 'fingerprint.submit', {
          'fingerprint_hash': fpHash,
          'peer_id':          rats.ownPeerId,
          'content_hash':     localHash,
          'title':            updated.title,
          'artist':           updated.artist,
          'genre':            updated.genre,
          'album':            updated.album,
          'year':             updated.year,
          'track_number':     updated.trackNumber,
          'bitrate':          updated.bitrate,
          'duration_ms':      updated.durationMs,
          'audio_format':     updated.audioFormat,
        }, timeout: const Duration(seconds: 8));
      } catch (_) {/* best effort */}
    }
    return out.path;
  }

  Future<Directory> _downloadsDir() async {
    final base = await getApplicationDocumentsDirectory();
    final dir  = Directory('${base.path}/downloads');
    if (!await dir.exists()) await dir.create(recursive: true);
    return dir;
  }

  Future<Directory> _piecePartialDir() async {
    final base = await _downloadsDir();
    final d    = Directory('${base.path}/.partial');
    if (!await d.exists()) await d.create(recursive: true);
    return d;
  }

  /// Best-effort multi-peer piece download. Returns null on any failure
  /// (NoPeer / integrity / RPC), and the caller falls back to the
  /// legacy stream.open path so the user still gets the file.
  ///
  /// Sources are seeded from the full node's `stream.open` swarm reply
  /// (peer ids we already know about); SwarmRegistry's DHT lookup runs
  /// inside the downloader and merges any extra peers it finds.
  ///
  /// On success, the bytes have been written to [finalCachePath],
  /// integrity-checked against [expectedHash], and the partial state
  /// has been cleaned up.
  Future<PieceDownloadResult?> _tryPieceDownload({
    required String expectedHash,
    required String canonicalHash,
    required String finalCachePath,
    void Function(int got, int total)? onProgress,
  }) async {
    if (!_ratsReady) return null;
    final rats = _rats!;

    // Ask the full node who's in the swarm for this content. We use the
    // canonical chain hash (not the variant hash) because that's what
    // the chain — and therefore the full node's SwarmIndex — keys on.
    // The reply's `peers` array carries each variant peer's individual
    // content_hash, which is what `audio.piece_get` should ask for to
    // get matching bytes back. Falling through here means the legacy
    // path will try again with its own probe.
    List<PeerSource> seeded = [];
    PieceManifest? manifest;
    try {
      final reply = await rats.request(
        ratsPeerId!, 'stream.open',
        {'content_hash': canonicalHash},
        timeout: const Duration(seconds: 8),
      );
      if (reply is Map) {
        final m = reply.cast<String, dynamic>();
        // Swarm Transfer v2: the full node serves the per-piece manifest next to
        // the swarm peer list so the downloader can verify each chunk on arrival.
        // Absent on older nodes → null → whole-file verification only.
        manifest = PieceManifest.fromJson(m['manifest']);
        // Only `peers` array matters here — `source: local` would mean
        // the full node has bytes, which the post-pivot arch never does.
        final peers = m['peers'];
        if (peers is List) {
          for (final p in peers) {
            String pid = '';
            String hash = '';
            if (p is String) {
              pid = p;
              hash = canonicalHash;
            } else if (p is Map) {
              pid = (p['peer_id'] as String? ?? '').trim();
              hash = (p['content_hash'] as String? ?? canonicalHash).trim();
            }
            if (pid.isEmpty) continue;
            // Only swarm peers whose advertised hash matches what we're
            // asking for can serve us. Different-hash variants would
            // hash-mismatch at integrity time, so skip them.
            if (hash.toLowerCase() != expectedHash) continue;
            seeded.add(PeerSource(
                peerId: pid, address: '', origin: 'full-node-swarm'));
          }
        }
      }
    } catch (_) { /* fall through; PieceDownloader can still try DHT */ }

    // (DHT un-nerf P0a) DO NOT early-return when the full node's swarm list
    // is empty. The DHT seeder lookup lives INSIDE PieceDownloader.run()
    // (SwarmRegistry.findSources), so returning here would skip the DHT in
    // exactly the scenario it exists for — VPS swarm map empty / full node
    // offline. Always construct the downloader; it throws NoPeerAvailable
    // only when BOTH the VPS-seeded list AND the DHT yield nothing, which we
    // catch below and fall through to the legacy path. (Paired with the P0b
    // dial fix in piece_downloader.dart — DHT sources are now actually
    // connected to; together they restore VPS-independent transfer.)
    try {
      final downloader = PieceDownloader(
        contentHash:    expectedHash,
        cacheDir:       await _piecePartialDir(),
        finalCachePath: finalCachePath,
        extraSources:   seeded,
        manifest:       manifest,
        onProgress:     onProgress,
        // Swarm Transfer v2: downloads are latency-insensitive, so fetch the
        // widest range (16 pieces = 4 MB) per swarm.fetch to amortize the
        // request RTT. With the per-seeder window=1, workers beyond the swarm
        // size idle, so 8 is plenty.
        config: const PieceDownloaderConfig(maxWorkers: 8, fetchPieces: 16),
      );
      return await downloader.run();
    } on NoPeerAvailableException {
      return null;
    } on IntegrityException {
      // Integrity failed — partial state already wiped by the
      // downloader. Falling back gives the user a chance to get the
      // bytes via the legacy path; if that also fails, the song just
      // isn't fetchable right now.
      return null;
    } catch (e) {
      // Any other failure is non-fatal here; the legacy path is still
      // available.
      // ignore: avoid_print
      print('[piece] _tryPieceDownload failed for '
            '${expectedHash.substring(0, 12)}…: $e');
      return null;
    }
  }

  // ---- Sessions -------------------------------------------------------

  Future<PlaySession> startSession(String contentHash, String playerAddress) async {
    // Reject empty / obviously-malformed inputs before we burn an RPC
    // round trip on something the chain will 400 anyway. content_hash
    // must be a 32-byte (64 hex char) value; player_address must decode
    // to 20 bytes (40 hex chars, optionally 0x-prefixed). Bouncing
    // bad inputs here surfaces a precise ArgumentError to the caller
    // instead of an opaque "RatsRpcException(http_400)".
    final ch = contentHash.trim();
    if (ch.length != 64 || !RegExp(r'^[0-9a-fA-F]+$').hasMatch(ch)) {
      throw ArgumentError.value(
          contentHash, 'contentHash', 'must be 64-char hex (32 bytes)');
    }
    final pa = playerAddress.trim();
    final paHex = pa.startsWith('0x') || pa.startsWith('0X')
        ? pa.substring(2) : pa;
    if (paHex.length != 40 || !RegExp(r'^[0-9a-fA-F]+$').hasMatch(paHex)) {
      throw ArgumentError.value(playerAddress, 'playerAddress',
          'must be 40-char hex address (optionally 0x-prefixed)');
    }
    // #5: attach the structural device attestation so the full node can key
    // its per-device mint limiter on hardware rather than a wallet. The
    // AcceptAllVerifier records device_id + level and accepts; a real
    // verifier would gate here without any client change.
    final attest = await DeviceFingerprintService.instance.get();
    final r = await _rpc('session.start', {
      'content_hash':   ch,
      'player_address': pa,
      'attestation':    attest.toJson(),
    });
    // Chain reply is {session_id, block_hash} — it does not echo back
    // content_hash. Inject the caller's value so the resulting
    // PlaySession carries the hash forward instead of defaulting to ''.
    final m = Map<String, dynamic>.from(r as Map);
    m['content_hash'] ??= ch;
    return PlaySession.fromJson(m);
  }

  Future<void> sendHeartbeat(String sessionId,
      {required int positionMs}) async {
    // Minimal payload — the full node uses its own wall clock for the
    // session timeline and ignores any client-supplied timestamp, and
    // the PCM-checksum field was never consumed. Keeping this tiny
    // matters: a Spotify-scale session fires one of these every 5 s
    // per concurrent listener, which is millions per second at scale.
    await _rpc('session.heartbeat', {
      'session_id':  sessionId,
      'position_ms': positionMs,
    });
  }

  Future<MintResult> completeSession(String sessionId,
      {String seederAddress = '', String miniNodeAddress = ''}) async {
    final r = await _rpc('session.complete', {
      'session_id': sessionId,
      // Per-stream reward lanes: the seeder (peer that served the bytes) and the
      // mini-node (relay) for this play, as 40-hex peer-ids == wallet addresses.
      // Reported HERE (not at start) because the serving peer isn't known until
      // streaming begins. Empty for a cached play -> node skips those lanes.
      if (seederAddress.isNotEmpty)   'seeder_address':    seederAddress,
      if (miniNodeAddress.isNotEmpty) 'mini_node_address': miniNodeAddress,
    });
    return MintResult.fromJson(Map<String, dynamic>.from(r as Map));
  }

  // Uploads removed: under the post-pivot architecture the player
  // fingerprints local files via LibraryScanner.scanOnce() and submits
  // each fingerprint via RatsClient.request(home, 'fingerprint.submit', ...).
  // Audio bytes never leave the player.

  // ---- DHT ------------------------------------------------------------

  Future<Map<String, dynamic>> getDhtPeers() async {
    final r = await _rpc('dht.peers', const {});
    return Map<String, dynamic>.from(r as Map);
  }
}
