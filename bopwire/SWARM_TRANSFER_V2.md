# Swarm Transfer v2 (relay-preserving)

Status: **in progress** (branch `swarm-transfer-v2`). This is the spec the phased
implementation follows. It supersedes the per-chunk `audio.piece_get` + base64
transfer path for both **streaming** and **album/track download**.

## Goal

Replace today's slow, fragile transfer (single source, one JSON+base64 RPC per
256 KB chunk, relayed through one VPS at ~1–2 MB/s) with a **multi-source,
pipelined, binary** transfer that scales with swarm size — **without removing the
relay**, so the `RelayRewardTx`-per-byte economics stay intact.

## Hard constraints (decided with the owner)

1. **Keep the relay.** Every byte still traverses a mini-node; relays still earn
   per forwarded byte. No direct P2P in this version.
2. **Manifest is off-chain, signed by the seeder.** No block-format change. The
   on-chain whole-file `content_hash` remains the ultimate root of trust.
3. **Burst cap = 4 Mbit/s per seeder→downloader flow** = 500 KB/s (named
   constant `kSeederFlowCapBytesPerSec = 500000`). Each seeder pushes at the FULL
   cap — not a polite fraction — and a downloader **aggregates N flows** via
   multi-source, so its speed = N × 4 Mbit/s, bounded only by its downlink. No
   per-stream "be nice" backoff.
4. **No per-destination / global relay cap.** Monopolizing a mini-node is fine;
   scale by adding mini-nodes, not by throttling tenants. (Remove the existing
   server-side politeness pacing and the global congestion-window ceiling.)

## Trust model (multi-source from untrusted wallets)

- Per song: `manifest = { content_hash, piece_size=256KB, total_size,
  piece_hashes:[sha256 per piece] }`, plus a seeder signature over `sha256(manifest)`.
- Piece hashes are **deterministic** from the file, so all honest seeders produce
  an identical manifest. A downloader fetches manifests from ≥2 seeders and takes
  the **consistent** one; the signature gives non-repudiation (a lying seeder is
  identifiable and bannable).
- Each incoming chunk is verified against its `piece_hashes[i]` on arrival → a bad
  chunk fails *its own* hash and is refetched elsewhere, instead of the whole file
  failing at the end with no idea which source lied (today's behavior).
- Backstop: assembled file's SHA-256 must still equal the on-chain `content_hash`.

## Wire protocol

- `stream.open` reply gains: `manifest` (or its hash + fetch hint) and a
  **per-holder relay route** (which mini-node each seeder is reachable through, so
  the swarm can be spread across multiple relays).
- New verb `swarm.fetch { session, piece_start, count }` → seeder streams `count`
  pieces as **binary `relay-bin` frames** `{session, piece_index, offset, len} +
  raw bytes`. No base64 (−33%). One RTT amortized over `count` pieces.
- Flow control: downloader keeps a **window** of outstanding ranges per seeder
  (per-seeder AIMD); seeder paces its push to the 4 Mbit/s per-flow cap. Receiver
  advertised-window provides backpressure (replaces fixed time pacing).

## Scheduler (player, `piece_downloader.dart`)

- Pull holder set from `stream.open` (SwarmIndex/DB2). Open sessions to K seeders
  (K≈4–8), each via its own mini-node where possible.
- Global needed-bitmap; hand each seeder a disjoint range queue. Track per-seeder
  throughput; drop+replace a stalled / hash-failing / slow seeder with a fresh one.
- **Endgame:** for the last few pieces, duplicate-request from the fastest seeders
  to kill tail-latency stalls.
- Two policies on one engine:
  - **Streaming:** next-needed pieces (playback head + buffer-ahead) from the
    fastest 1–2 seeders; small window → low time-to-first-audio.
  - **Download:** wide fan-out, out-of-order, large windows, endgame on the tail.

## Phases

1. **Manifest + per-piece verify** (safe, standalone): generate+sign manifest at
   ingest; serve via `stream.open`; player fetches, verifies signature + each
   piece. Keeps `audio.piece_get` as-is.
2. **Ranged binary push**: `swarm.fetch` + binary framing replacing
   `audio.piece_get`+base64; per-seeder window. Capability-negotiated; old path
   kept as fallback.
3. **Multi-source scheduler + endgame** in `piece_downloader.dart`.
4. **Throttle removal + AIMD/receiver-window backpressure**: drop server-side
   pacing ([player_server.dart] `kPaceEveryChunks`/`kPaceDelay`), drop global
   `_cwnd` ceiling ([piece_downloader.dart]), set per-flow seeder pacing to
   4 Mbit/s, keep the mini-node token bucket only as a high anti-flood safety valve.

## Touch points

- Player: `piece_downloader.dart`, `player_server.dart`, `rats_client.dart`,
  `swarm_registry.dart`, `node_client.dart`.
- Full node: `src/api/rats_api.cpp` (`stream.open` reply, manifest store/serve),
  `src/store/swarm.*`, `src/store/library_store.*`, ingestion (manifest gen+sign).
- Mini-node: `tools/mini_node.cpp` (binary session forwarding, loosen token bucket).
- Compatibility: NONE required (owner decision) — `swarm.fetch` fully replaces
  `audio.piece_get`; the network cuts over together like the DHT network-tag
  change. The base64 `audio.piece_get` path is removed once the downloader is
  switched.
