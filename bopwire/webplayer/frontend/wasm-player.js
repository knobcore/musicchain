/* wasm-player.js — low-latency streaming audio via WASM decoders + Web Audio.
 *
 * Bypasses <audio>'s conservative startup buffering: it fetches the swarm stream,
 * decodes chunks as they arrive (mpg123 / libFLAC / libvorbis, vendored in
 * decoders/), and schedules the PCM through Web Audio, starting at the first
 * frames. load() reads the Content-Type from the response and throws
 * "unsupported:" for anything but mp3/flac/ogg-vorbis, so the caller can fall
 * back to <audio>. Exposed as window.WasmPlayer.
 *
 * Concurrency: every load()/seek()/stop() claims a new generation (_gen) up
 * front, SYNCHRONOUSLY, so a rapid second click immediately invalidates the
 * first. Each load owns its OWN decoder (never a shared field a stale pump could
 * decode through, nor one another call could free mid-decode); the owning pump
 * frees it when it stops. This is what stops two tracks overlapping.
 */
(() => {
  'use strict';

  function decoderFactory(ct) {
    ct = (ct || '').toLowerCase();
    const g = window;
    if (/mpeg|mp3/.test(ct)   && g['mpg123-decoder'])     return () => new g['mpg123-decoder'].MPEGDecoder();
    if (/flac/.test(ct)       && g['flac-decoder'])       return () => new g['flac-decoder'].FLACDecoder();
    if (/ogg|vorbis/.test(ct) && g['ogg-vorbis-decoder']) return () => new g['ogg-vorbis-decoder'].OggVorbisDecoder();
    return null;
  }

  class WasmPlayer {
    constructor() {
      this.ctx = null; this.gain = null;
      this.url = ''; this._ct = ''; this.totalBytes = 0; this.durationSec = 0;
      this._basePos = 0; this._baseCtx = 0; this._next = 0;
      this._sources = []; this._abort = null;
      this._endTimer = null; this._tickTimer = null;
      this._gen = 0; this.playing = false; this._eof = false; this.volume = 1;
      this.onended = null; this.ontimeupdate = null; this.onplaying = null;
    }

    // Create + resume the AudioContext. MUST be called synchronously inside a
    // user gesture (a click) — browsers block audio started outside one. The ctx
    // is created once and reused across songs.
    unlock() {
      if (!this.ctx) {
        this.ctx = new (window.AudioContext || window.webkitAudioContext)();
        this.gain = this.ctx.createGain();
        this.gain.gain.value = this.volume;
        this.gain.connect(this.ctx.destination);
      }
      if (this.ctx.state === 'suspended') this.ctx.resume();
    }

    // Synchronously silence the current playback. Does NOT bump _gen and does NOT
    // free the decoder — the owning pump frees its own decoder when it stops.
    _silence() {
      this.playing = false;
      if (this._abort) { try { this._abort.abort(); } catch (_) {} this._abort = null; }
      if (this._endTimer)  { clearTimeout(this._endTimer);  this._endTimer = null; }
      if (this._tickTimer) { clearTimeout(this._tickTimer); this._tickTimer = null; }
      for (const s of this._sources) { try { s.stop(); } catch (_) {} }
      this._sources = [];
      this._next = 0; this._baseCtx = 0;
    }

    // Throws Error('unsupported:<ct>') if the codec isn't one we WASM-decode, so
    // the caller falls back to <audio>. Returns (no throw) if superseded by a
    // newer load/stop while awaiting. durationSec drives the seek bar + seeking.
    async load(url, durationSec) {
      const gen = ++this._gen;            // claim NOW, synchronously — supersedes any running load/pump
      this._silence();
      this.url = url; this._ct = ''; this.durationSec = durationSec || 0; this.totalBytes = 0;
      this.unlock();
      this.playing = true; this._eof = false;
      this._basePos = 0; this._baseCtx = 0; this._next = 0;
      this._abort = new AbortController();

      let resp;
      try { resp = await fetch(url, { signal: this._abort.signal }); }
      catch (e) { if (gen === this._gen) throw e; return; }   // aborted by supersede → swallow
      if (gen !== this._gen) { try { resp.body.cancel(); } catch (_) {} return; }

      const ct = resp.headers.get('Content-Type') || '';
      const make = decoderFactory(ct);
      if (!make) { try { resp.body.cancel(); } catch (_) {} throw new Error('unsupported:' + ct); }
      this._ct = ct;

      const decoder = make();             // OWN decoder for this generation
      try { await decoder.ready; } catch (_) {}
      if (gen !== this._gen) { try { decoder.free(); } catch (_) {} return; }

      this._readTotal(resp, 0);
      this._pump(resp.body.getReader(), gen, decoder);
      this._tick();
    }

    _readTotal(resp, offset) {
      const cr = resp.headers.get('Content-Range');
      const m = cr && cr.match(/\/(\d+)\s*$/);
      if (m) this.totalBytes = parseInt(m[1], 10);
      else {
        const cl = resp.headers.get('Content-Length');
        if (cl && !this.totalBytes) this.totalBytes = offset + parseInt(cl, 10);
      }
    }

    async _pump(reader, gen, decoder) {
      try {
        while (this.playing && gen === this._gen) {
          // Pace: stay within ~12 s of playback instead of scheduling the whole
          // song at once (the gateway streams faster than realtime). Bounds the
          // live source count so stop() can actually stop everything. Pausing the
          // reader also backpressures the fetch, so the gateway stops over-pulling.
          while (this.playing && gen === this._gen && this.ctx && this._next > 0 &&
                 (this._next - this.ctx.currentTime) > 12) {
            await new Promise((r) => setTimeout(r, 150));
          }
          if (!this.playing || gen !== this._gen) break;
          const { done, value } = await reader.read();
          if (done || gen !== this._gen) break;
          let out;
          try { out = await decoder.decode(value); } catch (_) { continue; }
          if (out && out.samplesDecoded > 0 && gen === this._gen) this._schedule(out);
        }
        if (gen === this._gen) { this._eof = true; this._armEnd(); }
      } catch (_) { /* aborted / stream error */ }
      finally {
        try { reader.cancel(); } catch (_) {}
        // The pump that owned this decoder is the only one allowed to free it,
        // and only after its loop has stopped issuing decode() calls.
        try { decoder.free(); } catch (_) {}
      }
    }

    _schedule(out) {
      const { channelData, samplesDecoded, sampleRate } = out;
      if (!sampleRate || !samplesDecoded || !channelData.length) return;
      const buf = this.ctx.createBuffer(channelData.length, samplesDecoded, sampleRate);
      for (let c = 0; c < channelData.length; c++)
        buf.copyToChannel(channelData[c].subarray(0, samplesDecoded), c);
      const src = this.ctx.createBufferSource();
      src.buffer = buf; src.connect(this.gain);
      if (this._next === 0) {
        this._baseCtx = this.ctx.currentTime + 0.06;     // tiny lead so we never schedule in the past
        this._next = this._baseCtx;
        if (this.onplaying) this.onplaying();
      }
      const t = Math.max(this._next, this.ctx.currentTime);
      try { src.start(t); } catch (_) { return; }
      this._next = t + buf.duration;
      // prune each buffer once it finishes so the live set stays small but COMPLETE
      // (every still-scheduled buffer is tracked, so _silence() silences all of them)
      src.onended = () => { const i = this._sources.indexOf(src); if (i >= 0) this._sources.splice(i, 1); };
      this._sources.push(src);
    }

    _armEnd() {
      if (this._endTimer) clearTimeout(this._endTimer);
      if (!this.ctx || this._next === 0) return;
      const ms = Math.max(0, (this._next - this.ctx.currentTime) * 1000) + 60;
      this._endTimer = setTimeout(() => { if (this.onended) this.onended(); }, ms);
    }

    _tick() {
      if (this._tickTimer) clearTimeout(this._tickTimer);
      this._tickTimer = setTimeout(() => {
        if (this.ontimeupdate) this.ontimeupdate();
        if (this.playing) this._tick();
      }, 200);
    }

    get currentTime() {
      if (!this.ctx || this._baseCtx === 0) return this._basePos;
      return Math.max(0, this._basePos + (this.ctx.currentTime - this._baseCtx));
    }
    get duration() { return this.durationSec; }
    get paused() { return !this.ctx || this.ctx.state !== 'running'; }

    pause()  { if (this.ctx && this.ctx.state === 'running')   this.ctx.suspend(); }
    resume() { if (this.ctx && this.ctx.state === 'suspended') this.ctx.resume(); }
    setVolume(v) { this.volume = v; if (this.gain) this.gain.gain.value = v; }

    async seek(posSec) {
      if (!this.ctx || !this._ct || !this.totalBytes || !this.durationSec) return;
      posSec = Math.max(0, Math.min(posSec, this.durationSec));
      const make = decoderFactory(this._ct);
      if (!make) return;

      const gen = ++this._gen;            // supersede the running pump (it frees its own decoder)
      this._silence();
      this.playing = true; this._eof = false;
      this._basePos = posSec; this._baseCtx = 0; this._next = 0;
      this._abort = new AbortController();

      const offset = Math.floor((posSec / this.durationSec) * this.totalBytes);
      let resp;
      try { resp = await fetch(this.url, { signal: this._abort.signal, headers: { Range: `bytes=${offset}-` } }); }
      catch (_) { return; }
      if (gen !== this._gen) { try { resp.body.cancel(); } catch (_) {} return; }

      const decoder = make();
      try { await decoder.ready; } catch (_) {}
      if (gen !== this._gen) { try { decoder.free(); } catch (_) {} return; }

      this._readTotal(resp, offset);
      this._pump(resp.body.getReader(), gen, decoder);
      this._tick();
    }

    async stop() {
      this._gen++;                        // supersede the running pump → it frees its own decoder
      this._silence();
      this._basePos = 0; this.totalBytes = 0;
      // keep this.ctx / this.gain alive for reuse (recreating needs a gesture)
    }
  }

  window.WasmPlayer = WasmPlayer;
})();
