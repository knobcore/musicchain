/* Bopwire web player — Discover. Vanilla JS, no framework.
 * Talks only to the gateway (window.BOPWIRE.gateway). Ports the native app's
 * Discover tab: Artist/Genre drill → albums → track list → stream.
 */
(() => {
  'use strict';
  const CFG = window.BOPWIRE;
  const $   = (id) => document.getElementById(id);

  // ───────────────────────── State ─────────────────────────
  const state = {
    songs:   [],          // raw catalog from the gateway
    mode:    'artist',    // 'artist' | 'genre'
    path:    [],          // drill: [{key,label}] (artist) or [{genre},{artist}]
    album:   null,        // {key,label} selected album → track pane
    query:   '',          // search text (overrides drill when non-empty)
    queue:   [],          // current track context for auto-advance
    qIndex:  -1,
    playing: null,        // contentHash currently playing
    playId:  null,        // active reward session id (gateway)
  };

  // ─────────────────────── Utilities ───────────────────────
  const esc = (s) => String(s ?? '').replace(/[&<>"']/g,
    (c) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const normKey = (s) => String(s ?? '').trim().toLowerCase().replace(/\s+/g, ' ');

  const fmtDur = (ms) => {
    const t = Math.max(0, Math.floor((ms || 0) / 1000));
    const m = Math.floor(t / 60), s = t % 60;
    return `${m}:${String(s).padStart(2, '0')}`;
  };
  const fmtPlays = (n) => {
    n = n || 0;
    if (n < 1000)   return `${n}`;
    if (n < 1e6)    return `${(n / 1e3).toFixed(1).replace(/\.0$/, '')}K`;
    return `${(n / 1e6).toFixed(1).replace(/\.0$/, '')}M`;
  };

  let toastTimer = null;
  function toast(msg) {
    const t = $('toast');
    t.textContent = msg; t.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { t.hidden = true; }, 3200);
  }

  // Group items by normalized key; label = most frequent original spelling.
  function bucketByNorm(items, keyFn) {
    const map = new Map();
    for (const it of items) {
      const raw = keyFn(it);
      const key = normKey(raw);
      let b = map.get(key);
      if (!b) { b = { key, items: [], spell: new Map() }; map.set(key, b); }
      b.items.push(it);
      const label = (raw ?? '').toString().trim();
      b.spell.set(label, (b.spell.get(label) || 0) + 1);
    }
    for (const b of map.values()) {
      let best = '', bestN = -1;
      for (const [sp, n] of b.spell) if (n > bestN) { best = sp; bestN = n; }
      b.label = best;
    }
    return map;
  }

  const streamable = (songs) => songs.filter((s) => (s.swarmSize || 0) > 0);

  // ─────────────────────── Gateway API ─────────────────────
  async function apiGet(path) {
    const r = await fetch(CFG.gateway + path, { mode: 'cors' });
    if (!r.ok) {
      let msg = `HTTP ${r.status}`;
      try { msg = (await r.json()).error || msg; } catch (_) {}
      throw new Error(msg);
    }
    return r.json();
  }
  const streamUrl = (hash) => `${CFG.gateway}/api/stream/${hash}`;

  async function apiPost(path, body) {
    const r = await fetch(CFG.gateway + path, {
      method: 'POST', mode: 'cors',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    return r.json();
  }

  // ───────────────────── Hierarchy logic ───────────────────
  function rootLabel() { return state.mode === 'artist' ? 'Artists' : 'Genres'; }

  function levelKind() {
    if (state.mode === 'artist') return state.path.length === 0 ? 'artist' : 'album';
    return ['genre', 'artist', 'album'][Math.min(state.path.length, 2)];
  }

  // Songs narrowed by the current drill path (not yet by album).
  function songsForPath() {
    let arr = streamable(state.songs);
    if (state.mode === 'artist') {
      if (state.path[0]) arr = arr.filter((s) => normKey(s.artist) === state.path[0].key);
    } else {
      if (state.path[0]) arr = arr.filter((s) => normKey(s.genre)  === state.path[0].key);
      if (state.path[1]) arr = arr.filter((s) => normKey(s.artist) === state.path[1].key);
    }
    return arr;
  }

  const albumLabelOf = (s) => (s.album && s.album.trim()) ? s.album.trim() : 'Singles';

  function facetChips() {
    const kind = levelKind();
    const arr  = songsForPath();
    const keyFn = kind === 'genre'  ? (s) => s.genre
                : kind === 'artist' ? (s) => s.artist
                : (s) => albumLabelOf(s);
    const buckets = [...bucketByNorm(arr, keyFn).values()];

    if (kind === 'album') {
      // earliest year per album for chronological-ish ordering, then name
      for (const b of buckets) {
        b.year = b.items.reduce((y, s) => (s.year && (!y || s.year < y)) ? s.year : y, 0);
      }
      buckets.sort((a, b) => (a.year || 9999) - (b.year || 9999) || a.label.localeCompare(b.label));
    } else {
      // alphabetical, unknown/empty to the end
      buckets.sort((a, b) => (!!b.key - !!a.key) || a.label.localeCompare(b.label));
    }
    return { kind, buckets };
  }

  // ───────────────────────── Render ────────────────────────
  function render() {
    renderModeToggle();
    renderBreadcrumb();
    if (state.query) { renderSearch(); }
    else { renderChips(); renderTrackPane(); }
  }

  function renderModeToggle() {
    document.querySelectorAll('#mode-toggle .seg').forEach((btn) => {
      btn.setAttribute('aria-selected', String(btn.dataset.mode === state.mode));
    });
  }

  function renderBreadcrumb() {
    const bc = $('breadcrumb');
    if (state.query) {
      bc.innerHTML = `<span class="crumb current">Search: “${esc(state.query)}”</span>`;
      return;
    }
    const crumbs = [{ label: rootLabel(), go: () => { state.path = []; state.album = null; } }];
    state.path.forEach((p, i) => crumbs.push({
      label: p.label, go: () => { state.path = state.path.slice(0, i + 1); state.album = null; },
    }));
    if (state.album) crumbs.push({ label: state.album.label, current: true });

    bc.innerHTML = crumbs.map((c, i) => {
      const last = i === crumbs.length - 1;
      const cls = (last || c.current) ? 'crumb current' : 'crumb';
      const sep = i > 0 ? '<span class="crumb-sep">›</span>' : '';
      return `${sep}<span class="${cls}" data-i="${i}">${esc(c.label)}</span>`;
    }).join(' ');

    bc.querySelectorAll('.crumb:not(.current)').forEach((el) => {
      el.onclick = () => { crumbs[+el.dataset.i].go(); render(); };
    });
  }

  function renderChips() {
    const wrap = $('chips'), empty = $('chips-empty');
    const { kind, buckets } = facetChips();
    if (!buckets.length) {
      wrap.innerHTML = ''; empty.hidden = false;
      empty.textContent = state.songs.length ? 'Nothing here right now.'
                                             : 'No streamable songs on the network yet.';
      return;
    }
    empty.hidden = true;
    const ico = kind === 'genre' ? '🏷' : kind === 'artist' ? '👤' : '💿';
    wrap.innerHTML = buckets.map((b, i) => {
      const n = b.items.length;
      const active = (kind === 'album' && state.album && state.album.key === b.key) ? ' active' : '';
      return `<button class="chip${active}" data-i="${i}">
        <span class="c-ico">${ico}</span>
        <span class="c-name">${esc(b.label)}</span>
        <span class="c-count">${n}</span>
      </button>`;
    }).join('');

    wrap.querySelectorAll('.chip').forEach((el) => {
      el.onclick = () => {
        const b = buckets[+el.dataset.i];
        if (kind === 'album') { state.album = { key: b.key, label: b.label }; }
        else { state.path.push({ key: b.key, label: b.label }); state.album = null; }
        render();
      };
    });
  }

  function renderTrackPane() {
    const head = $('track-head'), list = $('tracks'), empty = $('track-empty');
    if (!state.album) {
      head.hidden = true; list.innerHTML = ''; empty.hidden = false;
      empty.textContent = 'Pick an album to see its tracks.';
      return;
    }
    const tracks = songsForPath()
      .filter((s) => normKey(albumLabelOf(s)) === state.album.key)
      .sort((a, b) => (a.trackNumber || 9999) - (b.trackNumber || 9999)
                   || a.title.localeCompare(b.title));

    const year = tracks.reduce((y, s) => s.year || y, 0);
    head.hidden = false;
    head.innerHTML = `
      <div>
        <div class="th-title">${esc(state.album.label)}</div>
        <div class="th-sub">${tracks.length} track${tracks.length === 1 ? '' : 's'}${year ? ' · ' + year : ''}</div>
      </div>
      <button class="th-close" id="th-close">✕ close</button>`;
    $('th-close').onclick = () => { state.album = null; render(); };

    empty.hidden = true;
    renderTrackList(list, tracks, /*numbered=*/true);
  }

  function renderSearch() {
    $('chips').innerHTML = '';
    $('chips-empty').hidden = false;
    $('chips-empty').textContent = 'Showing search results below.';
    const head = $('track-head'), list = $('tracks'), empty = $('track-empty');
    const q = normKey(state.query);
    const hits = streamable(state.songs).filter((s) =>
      [s.title, s.artist, s.album, s.genre].some((f) => normKey(f).includes(q)))
      .sort((a, b) => (b.playCount || 0) - (a.playCount || 0));
    head.hidden = true;
    if (!hits.length) { list.innerHTML = ''; empty.hidden = false; empty.textContent = 'No matches.'; return; }
    empty.hidden = true;
    renderTrackList(list, hits, /*numbered=*/false);
  }

  function renderTrackList(list, tracks, numbered) {
    state.queue = tracks; // playing from a list sets the auto-advance context
    list.innerHTML = tracks.map((s, i) => {
      const playing = state.playing === s.contentHash ? ' playing' : '';
      const num = numbered ? (s.trackNumber || i + 1) : i + 1;
      return `<div class="track${playing}" data-i="${i}">
        <div class="t-num">${num}</div>
        <div class="t-main">
          <div class="t-title">${esc(s.title) || '(untitled)'}</div>
          <div class="t-artist">${esc(s.artist)}</div>
        </div>
        <div class="t-plays">${fmtPlays(s.playCount)} plays</div>
        <div class="t-dur">${fmtDur(s.durationMs)}</div>
        <div class="t-play">▶</div>
      </div>`;
    }).join('');
    list.querySelectorAll('.track').forEach((el) => {
      el.onclick = () => playFromQueue(+el.dataset.i);
    });
  }

  // ───────────────────────── Playback ──────────────────────
  // Two engines: the WASM decoder (low click-to-play latency, mp3/flac/ogg) and
  // the native <audio> element as a fallback for anything WASM can't decode.
  const audio = $('audio');
  const wasm  = window.WasmPlayer ? new window.WasmPlayer() : null;
  let engine  = 'audio';            // which engine is currently driving playback

  const engPaused = () => (engine === 'wasm' && wasm ? wasm.paused : audio.paused);
  const engCurSec = () => ((engine === 'wasm' && wasm ? wasm.currentTime : audio.currentTime) || 0);
  const engDurSec = () => (engine === 'wasm' && wasm ? wasm.duration : (audio.duration || 0));

  function npToggleIcon() { $('np-toggle').textContent = engPaused() ? '▶' : '⏸'; }
  function npProgress() {
    const cur = engCurSec(), dur = engDurSec();
    $('np-cur').textContent = fmtDur(cur * 1000);
    if (dur) { $('np-dur').textContent = fmtDur(dur * 1000); $('np-bar').value = Math.round((cur / dur) * 1000); }
  }

  function playFromQueue(i) {
    state.qIndex = i;
    play(state.queue[i]);
  }

  function play(song) {
    if (!song) return;
    // Silence whatever is playing RIGHT NOW, synchronously, before the async
    // startEngine for the new song runs — otherwise a quick second click leaves
    // the first track playing underneath the new one.
    audio.pause();
    try { audio.removeAttribute('src'); audio.load(); } catch (_) {}
    if (wasm) { wasm.unlock(); wasm.stop(); }   // unlock resumes the ctx in this gesture; stop silences the old song
    completePlay();                 // finalize the previous song's reward session
    state.playing = song.contentHash;
    $('nowplaying').hidden = false;
    $('np-title').textContent  = song.title || '(untitled)';
    $('np-artist').textContent = song.artist || '';
    $('np-cur').textContent = '0:00'; $('np-bar').value = 0;
    $('np-dur').textContent = fmtDur(song.durationMs || 0);
    $('np-spin').hidden = false;
    $('np-toggle').textContent = '⏸';
    startEngine(song);
    startPlay(song);                // open reward session (artist/seeder/mini mint)
    if (!state.query) renderTrackPane(); else renderSearch();
  }

  // Try the WASM decoder first; fall back to <audio> on unsupported codec, error,
  // or if no audio actually starts within a few seconds (watchdog).
  async function startEngine(song) {
    const url = streamUrl(song.contentHash);
    const durSec = (song.durationMs || 0) / 1000;
    audio.pause();
    try { audio.removeAttribute('src'); audio.load(); } catch (_) {}
    const useAudio = () => {
      if (state.playing !== song.contentHash) return;
      engine = 'audio';
      audio.src = url;
      audio.play().catch(() => {/* autoplay policy: user can hit play */});
    };
    if (wasm) {
      let started = false;
      wasm.onplaying    = () => { started = true; if (state.playing === song.contentHash) { $('np-spin').hidden = true; npToggleIcon(); } };
      wasm.ontimeupdate = () => { if (state.playing === song.contentHash) npProgress(); };
      wasm.onended      = () => { if (state.playing === song.contentHash) onTrackEnded(); };
      try {
        await wasm.load(url, durSec);
        if (state.playing !== song.contentHash) return;   // superseded by a newer click
        engine = 'wasm';
        setTimeout(() => {          // watchdog: no sound? drop to <audio>
          if (!started && engine === 'wasm' && state.playing === song.contentHash) {
            wasm.stop(); useAudio();
          }
        }, 5000);
        return;
      } catch (_) {                 // unsupported codec or decode error
        if (state.playing !== song.contentHash) return;   // a newer song is loading — don't touch it
        try { await wasm.stop(); } catch (_) {}
      }
    }
    useAudio();
  }

  function onTrackEnded() {
    if (state.qIndex >= 0 && state.qIndex + 1 < state.queue.length) {
      playFromQueue(state.qIndex + 1);   // play() finalizes the finished session
    } else {
      completePlay();
      state.playing = null; $('np-toggle').textContent = '▶'; render();
    }
  }

  // ── Reward session: the play earns for the artist/seeder/mini, never the
  // listener. The browser reports REAL playback progress so a mint only lands
  // on a genuine listen — same lifecycle as the native player.
  let hbTimer = null;
  function stopHeartbeat() { if (hbTimer) { clearInterval(hbTimer); hbTimer = null; } }
  async function startPlay(song) {
    stopHeartbeat(); state.playId = null;
    try {
      const r = await apiPost('/api/play/start', { contentHash: song.contentHash });
      if (state.playing === song.contentHash) {
        state.playId = r.playId;
        hbTimer = setInterval(() => {
          if (state.playId && !engPaused())
            apiPost('/api/play/heartbeat',
              { playId: state.playId, positionMs: Math.floor(engCurSec() * 1000) })
              .catch(() => {});
        }, 5000);
      }
    } catch (_) { /* reward best-effort; playback continues regardless */ }
  }
  function completePlay() {
    stopHeartbeat();
    const id = state.playId; state.playId = null;
    if (id) apiPost('/api/play/complete', { playId: id }).catch(() => {});
  }

  // <audio> events only drive the UI when it's the active engine.
  audio.addEventListener('playing',        () => { if (engine === 'audio') { $('np-spin').hidden = true; npToggleIcon(); } });
  audio.addEventListener('waiting',        () => { if (engine === 'audio') $('np-spin').hidden = false; });
  audio.addEventListener('pause',          () => { if (engine === 'audio') npToggleIcon(); });
  audio.addEventListener('play',           () => { if (engine === 'audio') npToggleIcon(); });
  audio.addEventListener('loadedmetadata', () => { if (engine === 'audio') $('np-dur').textContent = fmtDur(audio.duration * 1000); });
  audio.addEventListener('timeupdate',     () => { if (engine === 'audio') npProgress(); });
  audio.addEventListener('ended',          () => { if (engine === 'audio') onTrackEnded(); });
  audio.addEventListener('error',          () => {
    if (engine !== 'audio') return;
    $('np-spin').hidden = true;
    if (state.playing) toast('Could not stream this track — no seeders online right now.');
  });

  // Finalize the reward session if the tab is closed mid-play.
  window.addEventListener('pagehide', () => {
    if (state.playId && navigator.sendBeacon)
      navigator.sendBeacon(CFG.gateway + '/api/play/complete',
                           JSON.stringify({ playId: state.playId }));
  });

  $('np-toggle').onclick = () => {
    if (engine === 'wasm' && wasm) { wasm.paused ? wasm.resume() : wasm.pause(); setTimeout(npToggleIcon, 0); }
    else { audio.paused ? audio.play() : audio.pause(); }
  };
  $('np-bar').oninput = () => {
    const frac = +$('np-bar').value / 1000;
    if (engine === 'wasm' && wasm) { if (wasm.duration) wasm.seek(frac * wasm.duration); }
    else if (audio.duration) audio.currentTime = frac * audio.duration;
  };

  // ───────────────────── Data + status ─────────────────────
  async function refresh() {
    try {
      const songs = await apiGet('/api/songs');
      state.songs = Array.isArray(songs) ? songs : [];
      setStatus('ok', `${streamable(state.songs).length} songs online`);
      // prune a now-empty album/path selection so the view stays valid
      render();
    } catch (e) {
      setStatus('err', 'gateway unreachable');
      if (!state.songs.length) {
        $('chips-empty').hidden = false;
        $('chips-empty').textContent = 'Could not reach the network. Retrying…';
      }
    }
  }

  function setStatus(kind, text) {
    $('status-dot').className = 'dot ' + (kind === 'ok' ? 'ok' : kind === 'warn' ? 'warn' : 'err');
    $('status-text').textContent = text;
  }

  // ───────────────────────── Wiring ────────────────────────
  function wire() {
    document.querySelectorAll('#mode-toggle .seg').forEach((btn) => {
      btn.onclick = () => {
        if (state.mode === btn.dataset.mode) return;
        state.mode = btn.dataset.mode; state.path = []; state.album = null;
        render();
      };
    });

    let searchTimer = null;
    $('search').addEventListener('input', (e) => {
      clearTimeout(searchTimer);
      searchTimer = setTimeout(() => { state.query = e.target.value.trim(); render(); }, 150);
    });

    // CTA download dropdown
    const link = $('cta-link'), dd = $('cta-dropdown');
    document.querySelectorAll('.cta-opt').forEach((a) => { a.href = CFG.downloads[a.dataset.os] || '#'; });
    link.onclick = (e) => {
      e.stopPropagation();
      const open = dd.hidden;
      dd.hidden = !open; link.setAttribute('aria-expanded', String(open));
    };
    document.addEventListener('click', () => { dd.hidden = true; link.setAttribute('aria-expanded', 'false'); });

    // Draggable pane divider
    const divider = $('divider'), chipPane = $('chip-pane');
    let dragging = false;
    divider.addEventListener('pointerdown', (e) => { dragging = true; divider.setPointerCapture(e.pointerId); });
    divider.addEventListener('pointermove', (e) => {
      if (!dragging) return;
      const panes = $('panes').getBoundingClientRect();
      const pct = Math.min(80, Math.max(20, ((e.clientY - panes.top) / panes.height) * 100));
      chipPane.style.flexBasis = pct + '%';
    });
    divider.addEventListener('pointerup',  () => { dragging = false; });
  }

  // ───────────────────────── Boot ──────────────────────────
  function boot() {
    wire();
    render();
    refresh();
    setInterval(refresh, CFG.refreshMs);
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot);
  else boot();
})();
