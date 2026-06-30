# Gateway HTTP API

Base URL: `https://api.bopwire.com`. All JSON responses are UTF-8. CORS is open to
`https://bopwire.com` and `https://www.bopwire.com` (configurable). The browser never
sees librats, peer ids, relay routing, or wallets — the gateway hides all of it.

Internally each call maps to a librats RPC the native player already makes; that
mapping is noted per endpoint for reference.

---

### `GET /api/health`

Liveness + whether the gateway currently has a usable full node.

```json
{ "ok": true, "node": "connected", "songs_cached": 142, "uptime_s": 3600 }
```
`node` is `"connected"` once `routes.get` has yielded at least one full node, else
`"connecting"`. The browser uses this to show a "connecting to the network…" state.

---

### `GET /api/songs`

The Discover feed. Catalog of currently-streamable songs (`swarm_size > 0`, filtered
server-side, exactly like the app's Discover tab).

→ librats: `relay.forward → songs.list` to the selected full node.

```json
[
  {
    "contentHash": "9f86d0…",      // 64-hex, the stream key
    "title":       "Song Title",
    "artist":      "Artist Name",
    "album":       "Album",
    "genre":       "Rock",
    "year":        2024,            // 0 if unknown
    "trackNumber": 3,               // 0 if unknown
    "durationMs":  214000,
    "playCount":   1234,
    "swarmSize":   5                // peers currently serving; always > 0 here
  }
]
```
Field names are camelCase for the browser; the gateway translates from the node's
snake_case (`content_hash`, `track_number`, `swarm_size`, …).

---

### `GET /api/search?q=<text>` · `?artist=<name>` · `?genre=<name>`

Same item shape as `/api/songs`. → librats: `songs.search`.

---

### `GET /api/stream/<contentHash>`

The audio bytes for a song. Supports HTTP `Range` (seeking / progressive `<audio>`).

→ librats: `stream.open(content_hash)` → `swarm.fetch` ranges from seeders →
reassemble from binary frames → serve. `Content-Type` is sniffed from the container
(audio/mpeg, audio/flac, audio/ogg, audio/mp4, …); falls back to
`application/octet-stream`. The assembled file is cached (so a browser's range
re-requests and seeks don't re-pull the swarm), and concurrent requests for the same
song share one fetch.

- `200` full body (or `206 Partial Content` for a range request)
- `404` no seeders available right now (song fell out of the swarm)
- `503` gateway has no full node yet

This endpoint moves **bytes only** — it does not mint. Rewards are driven by the
play-session endpoints below, so a mint reflects a *real* listen, not a buffer fetch.

---

### Play session (the reward lifecycle)

The browser reports genuine playback, mirroring the native player. A web play mints
the **artist + seeder + mini** reward; the **listener never earns** (no wallet, no
signed receipt). The `player_address` sent on `start` is an ephemeral random id used
only to satisfy session attestation.

`POST /api/play/start` `{ "contentHash": "…" }` → `{ "playId": "<session_id>" }`
→ librats `session.start { content_hash, player_address=<random>, attestation:{} }`.

`POST /api/play/heartbeat` `{ "playId": "…", "positionMs": 41000 }` → `{ "ok": true }`
→ librats `session.heartbeat`. Sent every 5 s while actually playing.

`POST /api/play/complete` `{ "playId": "…" }` → MintResult
→ librats `session.complete { session_id, seeder_address, mini_node_address }`, the
seeder/mini taken from the fetch that served this song. The full node applies its own
≥50% effective-listen threshold before minting.

> Relay traffic is **load-balanced across the mini mesh** (`mininodes.list`), one mini
> pinned per play — the gateway never funnels everything through a single mini.

---

### Errors

Non-2xx JSON responses use `{ "error": "<message>" }`. The browser shows a toast and,
for the feed, keeps the last good catalog.
