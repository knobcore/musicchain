// Audio streaming bridge over the mini-node WebSocket gateway.
//
// The mini-node exposes a new `audio.fetch` verb on the same WebSocket
// the rest of the chain RPC rides on (port 8082 in the C++ wiring).
// Unlike a vanilla request → reply verb, `audio.fetch` is a one-to-many
// exchange:
//
//   client → text:  { req_id, type: "audio.fetch",
//                     body: { content_hash, peer_id? } }
//   server → text:  { req_id, status: "ok",
//                     body: { stream_id, total_bytes } }
//   server → bin*:  raw audio bytes (one or more frames; the gateway
//                   stripped the librats 9-byte stream/seq/eof header
//                   before forwarding, see audio_fetch_handler.h)
//   server → text:  { req_id, status: "complete",
//                     body: { sent } }
//
// The binary chunks may interleave across concurrent `audio.fetch`
// requests (e.g. the player prefetches the next song while the current
// one is still draining), which is why the server tags each chunk with
// its `stream_id` — the same demux trick librats uses for parallel
// swarm streams. AudioBridge keeps a registry of in-flight stream_ids,
// matches every incoming binary frame to its registry entry, and
// reassembles the bytes into a Blob the caller can hand to <audio>.src
// via URL.createObjectURL.
//
// The bridge installs itself onto two NodeClient hooks:
//   - `onBinaryFrame`     – every binary frame routes through `_onBytes`,
//                           which appends the entire frame (raw audio
//                           bytes, no wire header) to the currently-
//                           active in-flight entry. The gateway's
//                           rate-limit policy guarantees only one
//                           `audio.fetch` is in flight per WS, so we
//                           don't need a per-chunk stream id to demux.
//   - `onUnmatchedReply`  – the trailing `status: "complete"` envelope
//                           arrives AFTER NodeClient.request() already
//                           resolved on the initial `status: "ok"`. The
//                           hook catches it and triggers final Blob
//                           assembly.
//
// Only one AudioBridge should be installed per NodeClient at a time. If
// other subsystems need binary frames too, they'd need a multiplexing
// router on top — out of scope for today.
//
// Equivalent Dart side: musicchain_player/lib/src/services/rats_client.dart
// `_pullChunks` (which talks directly to librats rather than going through
// a WS gateway), and the streaming bits of node_client.dart.

import type { NodeClient } from './node_client';

/** Default ceiling for a single song fetch. Two paths reach this:
 *  - The verb's reply gives us `total_bytes`; we use that to size
 *    the progress callback and reject if the bytes exceed it.
 *  - If `total_bytes` is missing (older mini-node firmware) we fall
 *    back to this as a hard upper bound to avoid unbounded growth. */
const DEFAULT_MAX_BYTES = 64 * 1024 * 1024; // 64 MiB — chain policy is ~50 MiB.

/** Default timeout for an in-flight fetch, in ms. Generous because the
 *  songs come over the swarm and a cold start can spend seconds on the
 *  peer-handshake before bytes start flowing. */
const DEFAULT_FETCH_TIMEOUT_MS = 60_000;

const HASH_RE = /^[0-9a-fA-F]{64}$/;

/** Reply shape from the initial `audio.fetch` request. */
interface AudioFetchOpenReply {
  stream_id?: number;
  total_bytes?: number;
  /** Some mini-node builds echo the content hash; tolerated but unused. */
  content_hash?: string;
  /** Some builds advertise a MIME hint so the browser doesn't have to
   *  sniff. We pass it through to the caller untouched; if missing the
   *  caller assigns a sensible default. */
  mime?: string;
}

/** Body of the trailing `status: "complete"` envelope. */
interface AudioFetchCompleteBody {
  sent?: number;
}

/** What `fetchSong` resolves to once the last byte has landed. */
export interface AudioFetchResult {
  /** Reassembled bytes. Owned by the caller — keep a reference alive for
   *  the lifetime of `url`. */
  blob: Blob;
  /** `blob:`-scheme URL pointing at `blob`. Caller is responsible for
   *  calling `URL.revokeObjectURL(url)` when no longer needed. */
  url: string;
  /** Best-effort MIME type. Defaults to `audio/mpeg` when neither the
   *  server hint nor the caller supplied one — most songs in the chain
   *  are MP3-encoded so this gives <audio> the smallest hint surface
   *  it needs. */
  mime: string;
}

export interface FetchSongOptions {
  /** Optional swarm peer id to ask the mini-node to source from. When
   *  unset, the mini-node picks one out of its routing table. */
  peerId?: string;
  /** Progress callback fired as bytes arrive. `received` is cumulative,
   *  `total` is the `total_bytes` the server advertised (0 if unknown). */
  onProgress?: (received: number, total: number) => void;
  /** Override the default 60 s end-to-end timeout. */
  timeoutMs?: number;
  /** Override the default MIME applied to the resulting Blob. */
  mime?: string;
  /** Override the safety cap on total bytes. Defaults to 64 MiB. */
  maxBytes?: number;
}

/** Per-stream registry entry. One lives in `_inflight` for the duration
 *  of a single fetchSong call. */
interface StreamEntry {
  streamId: number;
  contentHash: string;
  totalBytes: number;
  /** Cumulative bytes appended to `chunks` so far. */
  received: number;
  /** Hard cap; we reject if this is exceeded. */
  maxBytes: number;
  chunks: Uint8Array[];
  mime: string;
  onProgress?: ((received: number, total: number) => void) | undefined;
  resolve: (result: AudioFetchResult) => void;
  reject: (err: Error) => void;
  timer: ReturnType<typeof setTimeout>;
  /** Set once the trailing `complete` envelope has fired. Used to
   *  no-op late-arriving binary frames during disposal. */
  finished: boolean;
}

/** Thrown for any fetch failure — open RPC error, bad framing, byte
 *  overflow, or final assembly mismatch. */
export class AudioFetchError extends Error {
  constructor(public readonly code: string, message: string) {
    super(message);
    this.name = 'AudioFetchError';
  }
}

/** Bridges the `audio.fetch` streaming verb on a NodeClient to Blob-
 *  delivered audio for the web player.
 *
 *  Usage:
 *    const node = new NodeClient();
 *    await node.connect();
 *    const bridge = new AudioBridge(node);
 *    const { url, blob, mime } = await bridge.fetchSong(hash, {
 *      onProgress: (r, t) => console.log(`${r}/${t}`),
 *    });
 *    audioEl.src = url;
 *    // when done:
 *    URL.revokeObjectURL(url);
 *
 *  The bridge installs hooks on `node.onBinaryFrame` and
 *  `node.onUnmatchedReply`. Call `dispose()` to release those hooks
 *  when the bridge is no longer needed. */
export class AudioBridge {
  private readonly _node: NodeClient;
  private readonly _inflight = new Map<number, StreamEntry>();
  /** Map req_id → stream_id so the trailing `complete` envelope (which
   *  carries the req_id but not the stream_id) can find the right
   *  registry entry. */
  private readonly _reqToStream = new Map<string, number>();

  /** Previously-installed hooks we'd restore on dispose() — saved so a
   *  second AudioBridge instance over the same NodeClient at least
   *  doesn't permanently clobber an unrelated callback. */
  private readonly _prevBinary: NodeClient['onBinaryFrame'];
  private readonly _prevUnmatched: NodeClient['onUnmatchedReply'];
  private _disposed = false;

  constructor(node: NodeClient) {
    this._node = node;
    this._prevBinary = node.onBinaryFrame;
    this._prevUnmatched = node.onUnmatchedReply;
    node.onBinaryFrame = (frame) => this._onBytes(frame);
    node.onUnmatchedReply = (env) => this._onUnmatchedReply(env);
  }

  /** Fetch the song bytes for `contentHash` over the mini-node gateway.
   *  Resolves with the reassembled Blob, an object URL, and the chosen
   *  MIME type. Rejects on RPC error, byte overflow, timeout, or
   *  abnormal termination. */
  async fetchSong(
    contentHash: string,
    opts: FetchSongOptions = {},
  ): Promise<AudioFetchResult> {
    if (this._disposed) {
      throw new AudioFetchError(
        'disposed', 'AudioBridge has been disposed',
      );
    }
    const ch = contentHash.trim();
    if (!HASH_RE.test(ch)) {
      throw new AudioFetchError(
        'bad_hash',
        `fetchSong: content_hash must be 64-char hex (got length ${ch.length})`,
      );
    }

    // Step 1 — discover a swarm peer the mini-node can stream from,
    // unless the caller already supplied peer_id. We do this even
    // though `audio.fetch` itself can auto-discover, because:
    //   (a) the mini-node's auto-discover only asks ONE full node
    //       and ONLY for 4 s — if that full node is mid-sync or
    //       behind on swarm-on/off events, the lookup returns no
    //       peer and `audio.fetch` fails with "no_peer".
    //   (b) we want the browser to see a useful error envelope
    //       ("no swarm members hosting this song right now") that
    //       names the song specifically, instead of the gateway's
    //       generic timeout.
    // The browser can call `stream.open` over the same WS gateway
    // because the gateway forwards it via relay.forward to a full
    // node — same path `songs.search` already takes successfully.
    let peerId: string | undefined = opts.peerId;
    if (peerId === undefined) {
      type StreamOpenReply = { peers?: Array<{ peer_id?: string }> };
      const streamOpen = await this._node.request<StreamOpenReply>(
        'stream.open',
        { content_hash: ch },
      );
      const peers = Array.isArray(streamOpen.peers) ? streamOpen.peers : [];
      const first = peers.find((p) =>
        typeof p?.peer_id === 'string' && /^[0-9a-fA-F]{40}$/.test(p.peer_id),
      );
      if (!first || !first.peer_id) {
        throw new AudioFetchError(
          'no_swarm',
          `no swarm peers currently hosting ${ch.substring(0, 16)}… — ` +
            `the player that uploaded this song may be offline`,
        );
      }
      peerId = first.peer_id;
    }

    // Step 2 — open the audio stream. The mini-node replies with the
    // stream_id we'll see on the binary frames + the total size we
    // should expect.
    const reqBody: { content_hash: string; peer_id?: string } = {
      content_hash: ch,
    };
    if (peerId !== undefined) reqBody.peer_id = peerId;
    const openReply = await this._node.request<AudioFetchOpenReply>(
      'audio.fetch',
      reqBody,
    );
    const streamId = openReply.stream_id;
    if (typeof streamId !== 'number' || !Number.isInteger(streamId) || streamId < 0) {
      throw new AudioFetchError(
        'bad_stream_id',
        `audio.fetch reply missing stream_id (got ${JSON.stringify(streamId)})`,
      );
    }
    if (streamId > 0xffff_ffff) {
      throw new AudioFetchError(
        'bad_stream_id',
        `audio.fetch stream_id out of u32 range: ${streamId}`,
      );
    }
    const totalBytes = typeof openReply.total_bytes === 'number'
      ? Math.max(0, Math.floor(openReply.total_bytes))
      : 0;
    const mime = opts.mime ?? openReply.mime ?? 'audio/mpeg';
    const maxBytes = opts.maxBytes ?? Math.max(totalBytes, DEFAULT_MAX_BYTES);
    if (totalBytes > maxBytes) {
      throw new AudioFetchError(
        'too_large',
        `audio.fetch total_bytes ${totalBytes} exceeds cap ${maxBytes}`,
      );
    }

    // Track the req_id so the trailing `complete` envelope can find
    // its entry. NodeClient.request() doesn't expose the req_id it
    // generated, so we ride the open envelope's own req_id by means
    // of the unmatched-reply hook: every `complete` we see is the
    // first matching stream_id from `_inflight`.
    //
    // The `_reqToStream` map intentionally stays empty for now: we
    // demultiplex on the unmatched envelope by walking the registry
    // and matching by stream_id field if the server echoes it, or by
    // first-in-first-out otherwise. The C++ side does echo stream_id
    // in `complete.body.stream_id` on current builds; we tolerate
    // either spelling.

    // Step 2 — install the per-stream entry and wait for bytes.
    return new Promise<AudioFetchResult>((resolve, reject) => {
      if (this._inflight.has(streamId)) {
        // Two concurrent fetchSong calls with the same stream_id can
        // only happen if the server reuses ids — protect ourselves.
        reject(new AudioFetchError(
          'stream_collision',
          `stream_id ${streamId} already in flight`,
        ));
        return;
      }
      const timeout = opts.timeoutMs ?? DEFAULT_FETCH_TIMEOUT_MS;
      const timer = setTimeout(() => {
        const entry = this._inflight.get(streamId);
        if (!entry || entry.finished) return;
        this._inflight.delete(streamId);
        entry.finished = true;
        entry.reject(new AudioFetchError(
          'timeout',
          `audio.fetch stream ${streamId} timed out after ${timeout}ms (${entry.received}/${entry.totalBytes} bytes)`,
        ));
      }, timeout);

      const entry: StreamEntry = {
        streamId,
        contentHash: ch,
        totalBytes,
        received: 0,
        maxBytes,
        chunks: [],
        mime,
        onProgress: opts.onProgress,
        resolve,
        reject,
        timer,
        finished: false,
      };
      this._inflight.set(streamId, entry);
    });
  }

  /** Release NodeClient hooks. Any in-flight fetchSong calls are
   *  rejected with `'disposed'`. */
  dispose(): void {
    if (this._disposed) return;
    this._disposed = true;
    // Restore whatever was there before.
    this._node.onBinaryFrame = this._prevBinary;
    this._node.onUnmatchedReply = this._prevUnmatched;
    const entries = Array.from(this._inflight.values());
    this._inflight.clear();
    this._reqToStream.clear();
    for (const e of entries) {
      if (e.finished) continue;
      clearTimeout(e.timer);
      e.finished = true;
      try {
        e.reject(new AudioFetchError(
          'disposed', 'AudioBridge disposed mid-stream',
        ));
      } catch (_) { /* swallow */ }
    }
  }

  /** True while any fetchSong call is in flight. Useful for UI spinners
   *  and shutdown sequencing. */
  get isStreaming(): boolean {
    return this._inflight.size > 0;
  }

  // -- Internals -------------------------------------------------------

  /** Handle one binary WS frame. The mini-node gateway sends RAW audio
   *  bytes — no stream_id prefix — because rate-limit policy on the
   *  gateway only permits one `audio.fetch` stream in flight per
   *  WebSocket connection (see docs/ws_gateway_rate_limits.md). So we
   *  just append every binary frame to whichever entry is currently
   *  in flight; if there's more than one (race window during teardown)
   *  the oldest one wins. Earlier builds of this file peeled a 4-byte
   *  "stream_id" header off the front of every frame, which
   *  corrupted the leading bytes of the actual audio (OGG header
   *  starts `OggS`, mp3 with ID3 or 0xFFFB sync — both got mangled). */
  private _onBytes(frame: ArrayBuffer): void {
    if (this._disposed) return;
    if (frame.byteLength === 0) return;
    // Pick the single in-flight entry. Iteration order on Map is
    // insertion order, so first() == oldest.
    let entry: StreamEntry | undefined;
    for (const e of this._inflight.values()) {
      if (!e.finished) { entry = e; break; }
    }
    if (!entry) {
      // No active fetch — chunk arrived after we finished, or before
      // the user kicked off a fetchSong. Drop.
      return;
    }
    // Copy into our own backing buffer; the underlying frame may be
    // recycled by ws/polyfilled WebSocket between event ticks.
    const owned = new Uint8Array(frame.byteLength);
    owned.set(new Uint8Array(frame));
    entry.chunks.push(owned);
    entry.received += owned.byteLength;
    if (entry.received > entry.maxBytes) {
      this._inflight.delete(entry.streamId);
      clearTimeout(entry.timer);
      entry.finished = true;
      entry.reject(new AudioFetchError(
        'overflow',
        `audio.fetch exceeded ${entry.maxBytes} bytes (got ${entry.received})`,
      ));
      return;
    }
    try { entry.onProgress?.(entry.received, entry.totalBytes); }
    catch (_) { /* user callback errors don't kill the stream */ }
    // Self-terminate when we've received the announced total. The
    // trailing "complete" envelope still arrives but is just a
    // formality at that point — finishing eagerly means the user
    // hears the song the instant the last byte lands instead of
    // waiting for the gateway's bookkeeping reply to round-trip.
    if (entry.totalBytes > 0 && entry.received >= entry.totalBytes) {
      this._finishEntry(entry, entry.received);
    }
  }

  /** Handle a trailing reply envelope (typically `status: "complete"`)
   *  whose req_id no longer has a pending request. */
  private _onUnmatchedReply(env: {
    req_id: string;
    status: string;
    body: unknown;
    error?: string;
  }): void {
    if (this._disposed) return;
    if (env.status === 'complete') {
      // Find the matching entry — prefer body.stream_id if the server
      // sends one (current C++ builds do), otherwise pick the oldest
      // entry whose receive count looks done.
      const body = (env.body && typeof env.body === 'object')
        ? env.body as Record<string, unknown> & AudioFetchCompleteBody & { stream_id?: number }
        : undefined;
      const explicit = typeof body?.stream_id === 'number' ? body.stream_id : undefined;
      const entry = explicit !== undefined
        ? this._inflight.get(explicit)
        : this._pickDoneEntry(body?.sent);
      if (!entry || entry.finished) return;
      this._finishEntry(entry, body?.sent);
      return;
    }
    if (env.status !== 'ok' && env.status !== undefined) {
      // Error envelope mid-stream — reject the matching entry. We don't
      // know which stream_id is implicated without an explicit hint, so
      // tear down everything that hasn't finished. This is a coarse
      // policy but errors are rare enough that abandoning queued
      // prefetches is the safe move.
      const body = (env.body && typeof env.body === 'object')
        ? env.body as { stream_id?: number }
        : undefined;
      const explicit = typeof body?.stream_id === 'number' ? body.stream_id : undefined;
      const reason = env.error || `audio.fetch failed: ${env.status}`;
      if (explicit !== undefined) {
        const entry = this._inflight.get(explicit);
        if (entry && !entry.finished) this._failEntry(entry, env.status, reason);
        return;
      }
      const entries = Array.from(this._inflight.values());
      for (const e of entries) {
        if (e.finished) continue;
        this._failEntry(e, env.status, reason);
      }
    }
  }

  /** Pick the oldest in-flight entry whose received count matches the
   *  `sent` value the server reported. Fallback when the server doesn't
   *  echo stream_id in the `complete` envelope. */
  private _pickDoneEntry(sent: number | undefined): StreamEntry | undefined {
    const entries = Array.from(this._inflight.values()).filter((e) => !e.finished);
    if (entries.length === 0) return undefined;
    if (typeof sent === 'number') {
      const exact = entries.find((e) => e.received === sent);
      if (exact) return exact;
    }
    // Otherwise the oldest entry — Map iteration order is insertion order.
    return entries[0];
  }

  private _finishEntry(entry: StreamEntry, sent: number | undefined): void {
    this._inflight.delete(entry.streamId);
    clearTimeout(entry.timer);
    entry.finished = true;
    // Light sanity check: if the server told us `sent` and it disagrees
    // with what we counted, we still hand back the bytes (the caller
    // can decide whether to retry) but flag the mismatch in the
    // resulting Blob's type field would be confusing — we just throw.
    if (typeof sent === 'number' && sent !== entry.received) {
      entry.reject(new AudioFetchError(
        'size_mismatch',
        `audio.fetch sent ${sent} bytes, browser counted ${entry.received}`,
      ));
      return;
    }
    if (entry.totalBytes > 0 && entry.received !== entry.totalBytes) {
      entry.reject(new AudioFetchError(
        'size_mismatch',
        `audio.fetch total_bytes ${entry.totalBytes}, browser counted ${entry.received}`,
      ));
      return;
    }
    const blob = new Blob(entry.chunks as BlobPart[], { type: entry.mime });
    // Drop our reference to the chunks now that the Blob owns them —
    // helps the GC reclaim the intermediate Uint8Arrays.
    entry.chunks.length = 0;
    const url = URL.createObjectURL(blob);
    try {
      entry.resolve({ blob, url, mime: entry.mime });
    } catch (_) { /* swallow — caller's then-handler error is theirs */ }
  }

  private _failEntry(entry: StreamEntry, code: string, message: string): void {
    this._inflight.delete(entry.streamId);
    clearTimeout(entry.timer);
    entry.finished = true;
    entry.chunks.length = 0;
    try {
      entry.reject(new AudioFetchError(code, message));
    } catch (_) { /* swallow */ }
  }
}
