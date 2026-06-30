# Bopwire Architecture

Canonical reference for the post-2026-06-21 topology: web/browser surfaces deleted,
librats frozen at v0.2.0, mini-nodes hardened as relays, full nodes as chain authority
and content trackers, players as Android + Windows only.

**Consensus is Model 1 — vote-free deterministic** (post-2026-06-24 de-nerf pass): blocks
carry NO validator confirmations; every node independently re-derives validity from content
+ history and converges on the heaviest chain, where "heaviest" = **cumulative audited plays**
(`MintTx` count, so free heartbeat blocks add 0 weight). MC is an **internal-only token** —
there is no external bridge (the Base / mcCOIN bridge was removed for its custodial,
forge-to-cash security model). The full blockchain + validation internals, the Model 1
design, and the de-nerf changelog live in
[`docs/BLOCKCHAIN_AND_NETWORK_INTERNALS.md`](docs/BLOCKCHAIN_AND_NETWORK_INTERNALS.md).

This file describes what is actually on disk, not what we wish were on disk. When
code disagrees with prose, code wins; if you change one, change the other.

---

## 1. Roles

Three node binaries, three jobs. No node fills more than one of these roles.

### Mini-nodes — `bopwire-mini-node`

VPS-resident routers. They tunnel JSON RPC + binary audio traffic in both
directions between players and full nodes when ICE hole-punch cannot reach the
target (cellular symmetric NAT, restrictive corporate firewall, residential
double-NAT). They run no chain code; they hold no audio bytes. Their state is
purely transient: a routing table of full nodes (`g_routes`), a peer-table of
fellow mini-nodes (`g_mininode_peers`), a player registry (`g_players`), an
in-memory chat ring per room, and a per-peer relay rate-limit bucket.

Mini-nodes earn `RelayRewardTx` credits for the traffic they tunnel. They
identify themselves to every fresh peer with `mini.hello` carrying their
EIP-55 wallet address (`tools/mini_node.cpp:696-712`), and the receiving full
node persists the binding via `peer_to_wallet_` (`src/api/rats_api.cpp:469-470`).

Mini-nodes load-balance with each other by gossiping their `load_score` in
every `mini.hello` ack and in the `mininodes.list` reply (`tools/mini_node.cpp:1055-1086`).
Players pick the lightest VPS for relay traffic via `bestMiniNodePeerId`
(`bopwire_player/lib/src/services/rats_client.dart:179-198`).

Bootstrap: `--peer-vps host:port[,...]` seeds the mesh from CLI
(`tools/mini_node.cpp:1682-1696`); thereafter every received route from a
non-mini-node peer is re-broadcast to every other mini-node
(`tools/mini_node.cpp:743-759`), and a freshly-meshed mini-node gets a
full-route snapshot on first contact (`tools/mini_node.cpp:1027-1037`).

### Full nodes — `bopwire-node`

Chain authority + content trackers + servers for everything that needs durable
state (catalog, swarm membership, moderation log, DMCA/KYC inbox, offline
play-proof verification, username registration, royalty session bookkeeping,
encrypted moderator inbox).

Full nodes run the chain (`mc::Chain`) + block distribution (`BlockPropagator`) and
persist a LevelDB at `<data_dir>/blockchain.db`. Consensus is **vote-free (Model 1)**:
a block is built → `connect_block`-validated → announced, with **no candidate/confirmation
gossip** (that machinery — `BlockCandidate`/`dynamic_quorum`/`mc:block_candidate`/
`mc:block_confirmation` — was deleted). Peers fetch blocks via the `block.*` verbs
(`block.hello`/`getblocks`/`inv`/`getdata`/`data` + per-block DHT announce) and re-validate
them deterministically. Fork choice adopts the chain with the most **cumulative audited
plays** (`ChainTip.weight`, `cw:` index), with an automatic try-and-rollback reorg
(`Chain::reorg_to_branch`). They expose the entire RPC verb surface in `src/api/rats_api.cpp`.

Full nodes discover content holders for a given song two ways:

1. **In-memory SwarmIndex** — every `fingerprint.submit` or `swarm.hello` from
   a player records `(content_hash → peer_id)` in `swarm_`
   (`src/api/rats_api.cpp:681-696` and `:895-905`). Connection-state authoritative:
   on `on_peer_disconnected_cb` the entry is hard-evicted (`:148-173`).
2. **librats DHT** — every successful `fingerprint.submit` triggers
   `rats_announce_for_hash(sha1(content_hash))` so the full node is reachable
   as a "tracker" for that song on the BitTorrent-compatible DHT
   (`src/api/rats_api.cpp:689-696` and `:786-793`). Players locate seeders by
   probing the same 20-byte SHA-1 key (`bopwire_player/lib/src/services/rats_client.dart:683-760`).

Full nodes load-balance with each other: each one re-publishes its route every
5 minutes (`src/network/rats_link.cpp:358-407`), and mini-nodes serve the
union of all currently-known full-node routes in `routes.get`. Players
multiplex requests across whichever full nodes are reachable.

### Players — Android + Windows

Outbound librats clients. No browser per [feedback-no-web-version]. Stream and
download audio peer-to-peer through other players' `audio.piece_get` /
`stream.open` handlers (`bopwire_player/lib/src/services/player_server.dart:108-114`).
A player on cellular (symmetric NAT) routes everything through a mini-node; a
player on open or port-forwarded desktop attempts direct rats_connect first
and falls back to relay only on failure.

Players also originate chain transactions (uploads via `fingerprint.submit`,
username registration via `username.register`, royalty sessions via
`session.start/heartbeat/complete`, offline play proofs via
`offline.play_proof.submit`).

---

## 2. Wire surfaces

What each role accepts inbound.

### Mini-node

- **librats TCP `rats_port`** — default 8080 (`tools/mini_node.cpp:53`,
  matches librats v0.2.0 project default).
- No browser-facing ports. The WsTcpRelay / WsAudioBridge / WsMiniGateway code
  was deleted (`tools/mini_node.cpp:1894-1898`).
- No HTTP/3, no HTTP, no JSON-RPC-over-WebSocket. Every verb arrives as a
  librats typed message of type `bopwire.request`; every reply leaves as
  `bopwire.reply`.

### Full node

- **librats TCP `rats_port`** — default 8080 (`src/network/manager.h:25`).
  Carries chain + swarm + content verbs (see §3, §4).
- **`api_port`** — default 9334 (`src/network/manager.h:24`). Retained in
  `NodeConfig` for config-file compatibility with older deploys; today it is
  embedded in route advertisements (`src/network/rats_link.cpp:148-172`) so
  any future HTTP/REST consumer knows where the node would listen, but no
  in-tree code opens a listening socket on it.
- **`p2p_port`** — default 9333. Same compat-only status as `api_port`
  (`src/network/manager.h:23` and the comment block above it).

### Player

- **librats TCP** — ephemeral port the OS picks (33000–34999 random by default,
  `bopwire_player/lib/src/services/rats_client.dart:96`). The DHT runs on a
  separate ephemeral UDP socket (`:42-43`). Neither port is advertised to peers
  reachable only via the relay path; other players locate this player by its
  `rats_peer_id`, not by `host:port`.
- No HTTP listener. The legacy `node_client.dart` HTTP surface was retained
  only for direct LAN dev nodes; production paths run through `RatsClient`.

---

## 3. Data plane (audio + catalog)

The path bytes take from "user taps play" to "audio decodes locally."

```
                          +-------------------+
                          |   Player (you)    |
                          +---------+---------+
                                    |
              songs.list /          |              audio.piece_get  /
              songs.search /        |              stream.open
              songs.get /           |              (continuous chunks)
              stream.open           v
                          +-------------------+              +-----------------+
                          |    Full node      |              |  Swarm peer     |
                          |  (catalog + swarm |              | (another player |
                          |   index)          |              |  that has the   |
                          +---------+---------+              |  bytes)         |
                                    ^                        +--------+--------+
                                    |                                 ^
                                    +--- swarm.hello / fingerprint -- +
                                                                      |
                          DHT find_peers(sha1(content_hash))----------+
```

- **Catalog lookup** — `songs.search`, `songs.list`, `songs.get` are all served
  by the full node (`src/api/rats_api.cpp:319-359`). The full node injects the
  live `swarm_size` next to each `songs.list` entry from its SwarmIndex so the
  client can hide songs nobody is currently serving (`:325-343`).
- **Swarm resolution** — `stream.open(content_hash)` is served by the full
  node and returns a variant-aware peers array
  (`src/api/rats_api.cpp:483-531`). Each peer entry carries that holder's
  *local* `content_hash`, `bitrate`, and `audio_format` so the requester can
  pick a quality.
- **Resumable download (multi-peer)** — `audio.piece_get(content_hash, offset,
  length, v:1)` is served peer-to-peer by `PlayerServer._handlePieceGet`
  (`bopwire_player/lib/src/services/player_server.dart:116-160`). The
  reply carries base64-encoded bytes capped at 512 KB
  (`:103`). PieceDownloader fans the verb across multiple swarm peers in
  parallel, reassembles in order, and writes through a single
  `RandomAccessFile`+bitmap pair so an interrupted download resumes byte-
  exactly (`bopwire_player/lib/src/services/piece_downloader.dart:482-549`).
- **Streaming download (continuous)** — `stream.open(content_hash)` peer-to-peer
  (NOT the same verb as the full-node catalog lookup; differentiation is by who
  the request is routed to). PlayerServer answers with a freshly-allocated
  `stream_id` and then pushes binary chunks
  (`bopwire_player/lib/src/services/player_server.dart:175-294`). Crucially,
  chunks are read from disk via `RandomAccessFile.read(n)` inside the send loop
  rather than `readAsBytes()` upfront — large files no longer load into RAM
  (`:264-292`). 16 KB payload per chunk (`:87`), paced at 4 chunks per 8 ms
  (~8 MB/s, `:96-97`).
- **DHT** — `rats_find_peers(sha1Hex)` runs over librats's BEP-5-compatible
  DHT. Full nodes announce themselves on `sha1("bopwire-fullnode-mainnet")`
  for general bootstrap, and per-song on `sha1(content_hash)` after every
  `fingerprint.submit` (`src/api/rats_api.cpp:182-191`, `:689-696`). Players
  resolve the per-song key with a long-lived NativeCallable + heap-owned
  result set (`bopwire_player/lib/src/services/rats_client.dart:663-760`),
  defending against the use-after-free that crashed older builds — see the
  comment block at `:645-661`.

---

## 4. Control plane (routing + discovery)

The non-data verbs that keep the mesh coherent.

```
        +--------+ mini.hello (load_score, wallet) +--------+
        |  mini  |<------------------------------>|  mini  |
        | node A |                                | node B |
        +--------+                                +--------+
            ^                                          ^
            | routes.get                               |
            | mininodes.list                           |
            | player.announce                          |
            | relay.forward                            |
            | F-tag binary relay                       |
            |                                          |
        +---+----+   relay.push.forward     +----------+----+
        | Player |<-------------------------|  Full node    |
        +--------+                          +---------------+
```

- **`mini.hello`** — bidirectional mini-node identity probe. Body carries the
  sender's wallet (`tools/mini_node.cpp:696-699`) and live load metrics
  (`:700-704`). The receiver inserts the sender into `g_mininode_peers`,
  records the score in `g_mininode_load`, snapshots its full route table back
  to the sender (`:1027-1037`). Full nodes treat the same verb as a wallet-
  binding announcement so relay credits land on the right address
  (`src/api/rats_api.cpp:458-478`).
- **`routes.get`** — player → mini-node. Returns the full node routing table
  with `load_score`, `cpu_load`, `net_bps`, `is_busy`, `reachability`, and
  the mini-node's own load snapshot
  (`tools/mini_node.cpp:388-439`, `:993-997`). Always targeted at a peer that
  self-identified as a mini-node via `mini.hello` — see the comment at
  `bopwire_player/lib/src/services/rats_client.dart:1276-1288`.
- **`mininodes.list`** — player → mini-node. Returns every fellow mini-node
  the responder knows about, plus itself, each with `load_score` and
  `public_address` (`tools/mini_node.cpp:1043-1086`). Players merge this into
  their local set, dial new entries on the spot, and persist it across runs
  via SharedPreferences (`bopwire_player/lib/src/services/rats_client.dart:478-527`).
- **`relay.forward`** — player → mini-node. Wraps an inner verb +
  `target_peer_id` so the mini-node forwards the inner envelope on its
  existing QUIC link (`tools/mini_node.cpp:1317-1411`). The mini-node mints a
  fresh `req_id`, stores the original under `g_pending_relays`, and routes
  the eventual reply back via `on_relay_reply` (`:1424-1474`). Send-side
  failure evicts the target's route immediately and surfaces `dead_route` to
  the originator (`:1379-1408`), avoiding wasted 15 s timeouts.
- **`relay.push.forward`** — full node → mini-node. Fire-and-forget
  notification path back to a player (`tools/mini_node.cpp:1277-1316`).
  Wrapped as a normal `bopwire.reply` with the inner type set verbatim
  so the receiver sees it on its push channel without `req_id` correlation.
- **F-tag binary relay** — player → mini-node. Wire format: `0x46` (`'F'`) +
  40 hex chars of `target_peer_id` + **16 raw `delivery_id` bytes** (#10, zeros if none) +
  payload. Mini-node strips the full `1+40+16` prefix, charges the payload to the rate
  bucket, accumulates the bytes against the `delivery_id` for the relay-reward triangulation
  (§5), and calls `rats_send_binary` against the target (`tools/mini_node.cpp on_relay_binary`).
  The `1+40+16` prefix length moves in **lockstep** across the player writer
  (`player_server.dart _streamChunks`) and the mini stripper — change one, change both, or the
  receiver reads its chunk header off the wrong offset. Rate-limited per source peer via a
  50 MB token bucket refilling at 10 MB/s, so a chatty cellular peer can't saturate the uplink.
- **`stun.observe`** — player → mini-node. Echoes the IP:port the responder
  sees the caller from, using `source_port` (NAT-mapped) not `port`
  (handshake-claimed listen port) — see the comment at
  `tools/mini_node.cpp:1140-1150`.
- **`player.announce` + `player.locate`** — player registers its NAT-mapped
  address, other players resolve it before attempting direct rats_connect
  (`tools/mini_node.cpp:1165-1232`). First-seen announce broadcasts
  `swarm.peer_online` to every full node so Discover surfaces update without
  waiting for the next `swarm.hello` tick (`:1190-1196`).
- **`ice.connect_request` / `ice.connect_invite`** — caller asks the mini-node
  to invite `target_pid` to hole-punch back to its public address
  (`tools/mini_node.cpp:1242-1276`).
- **`swarm.hello_digest` / `swarm.hello` / `swarm.add` / `swarm.remove`** —
  player → full node, efficient swarm sync. Digest preflight is a 96-byte
  round-trip when the library hasn't changed (`src/api/rats_api.cpp:834-862`).
  Full hello replaces the peer's set and returns unknown content_hashes that
  need a follow-up `fingerprint.submit` (`:864-906`). Deltas via add/remove
  (`:907-949`).
- **`swarm.peer_online` / `swarm.peer_offline`** — mini-node → full node.
  Carries player up/down state for relayed peers the full node has no
  direct callback for (`src/api/rats_api.cpp:958-984` and
  `tools/mini_node.cpp:767-848`).
- **Block distribution** — every verb prefixed `block.` is delegated to
  `BlockPropagator` (`src/api/rats_api.cpp:310-318`). `block.hello`,
  `block.getblocks`, `block.inv`, `block.getdata`, `block.data`. New peers
  get a `block.hello` immediately on connect (`src/api/rats_api.cpp:141-144`)
  so a behind-the-tip full node converges fast.
- **`mod.sync_since`** — replays moderation envelopes newer than the
  caller's last known timestamp. Triggered automatically on every fresh
  full-node peer connection (`src/api/rats_api.cpp:121-135`).

---

## 5. Earning model

MC is **internal-only** (no external bridge), so reward integrity is about fair internal
distribution, not external value. The relay reward is now credited **per corroborated byte**
via a **player/mini-node/full-node triangulation** (the old "one credit per `stream.open`,
founder trusts the relay" model is gone). Full design + exact wire formats:
[`docs/BLOCKCHAIN_AND_NETWORK_INTERNALS.md`](docs/BLOCKCHAIN_AND_NETWORK_INTERNALS.md) §19.1.

Mini-nodes earn `RelayRewardTx` for the bytes they actually tunnel, established by three
independent signed reports that must agree:

- **Broker mint** — at `stream.open` the full node mints a random 16-byte `delivery_id`,
  records a `pd:<id>` pending-delivery row, and returns the id on the reply
  (`RatsApi::mint_delivery`). The full node is the anchor: it provably brokered the request.
- **F-frame carries the id** — the requesting player threads `delivery_id` into its per-peer
  `stream.open`; the serving player stamps the 16 raw bytes into the relay F-frame prefix
  (`'F'`(1) + target(40) + **delivery_id(16)** + chunk, §4). The mini-node strips `1+40+16`,
  charges the payload to its rate bucket, and accumulates bytes per `delivery_id`.
- **Mini report (signed)** — on idle the reaper broadcasts a `relay.report` to every full
  node in `g_routes`; only the broker holds the matching `pd:` row and accepts it (others
  reply `ignored`). Signed `"relay.report" || delivery_id || bytes_relayed(LE) || mini_wallet`.
- **Player receipt (signed)** — the requesting player sends `relay.receipt` to the broker:
  `"relay.receipt" || delivery_id || content_hash || bytes_received(LE)`, signed with the
  player wallet (broker verifies with `verify_data`).
- **Credit on corroboration** — when brokered+reported+receipted all set, the broker credits
  `min(relayed,received)` bytes × **10 internal units** (1 MC / 10 MB, overflow-clamped) to
  the mini wallet via `RelayCreditTracker::increment`, then deletes the `pd:` row (single-use
  ⇒ replay-proof). Orphan rows are TTL-pruned in the tracker sweep.
- **Storage / sweep** — credits accrue in LevelDB under `rc:` (survives restarts); every
  `kSweepIntervalMs` (5 min) the tracker mints one `RelayRewardTx{count = internal units}` per
  mini, founder-signed, into the mempool. `apply_relay_reward` credits `count` **directly as
  units** (no per-MC scaling) under the `SUPPLY_CAP` guard, with a `kMaxUnitsPerTx = 1e12` cap
  the tracker pre-clamps against. Transaction layout: `src/core/transaction.h` (RelayRewardTx).

---

## 6. NAT model

How the player decides whether to dial direct or wrap as `relay.forward`.

- **Cellular** — symmetric NAT per [project-cellular-symmetric-nat]. Direct
  hole-punch never succeeds because the cellular CGNAT remaps source ports
  per destination 5-tuple. The player tries the direct path, sees no
  reachability flip, and falls back to the relay path. Every outbound RPC
  is wrapped in `relay.forward` via `bestMiniNodePeerId`
  (`bopwire_player/lib/src/services/rats_client.dart:849-880`). Binary
  audio sends prepend the F-tag.
- **Cone NAT (home Wi-Fi)** — possibly direct. After the mini-node observes
  the player's public address via `stun.observe`
  (`bopwire_player/lib/src/services/rats_client.dart:600-623`), the
  player can attempt `rats_connect(public_address)` against another player's
  observed address. Per [project-bopwire-platforms] this is the desktop
  fallback path.
- **Open / port-forwarded** — direct. The player observes its own peer
  appear with `reachability:"direct"` in the mini-node's `routes.get`
  response (which probes back on a fresh ephemeral source port,
  `tools/mini_node.cpp:441-502`). LibratsDiscovery uses that flag to drop
  the relay-via mapping (`bopwire_player/lib/src/services/rats_client.dart:200-214`).

---

## 7. Constraints + memory references

Things that bound the design and are non-negotiable inside this codebase.

- **librats is FROZEN at v0.2.0** (commit `246557c`) per
  [feedback-dont-modify-librats]. The vendored copy lives at `deps/librats/`
  and must NOT be patched. Limitations are wrapped at the call site —
  `RatsLink` (`src/network/rats_link.cpp`), `RatsApi` (`src/api/rats_api.cpp`),
  `RatsClient` (`bopwire_player/lib/src/services/rats_client.dart`). The
  duplicate-peer-storm fix, the heap-strdup peer-id contract, the binary
  callback `malloc` contract — all of these are now ASSUMED in the wrapper
  code (see `rats_client.dart:645-661`, `:1163-1183`); the wrapper frees
  per librats's strdup-and-handoff convention. If those contracts ever
  change upstream, every wrapper has to be re-audited at the same time.
- **No web/browser surfaces** per [feedback-no-web-version]. The
  `WsTcpRelay` / `WsAudioBridge` / `WsMiniGateway` classes were removed from
  the mini-node binary (`tools/mini_node.cpp:1894-1898`). Do not add a
  WebSocket gateway, browser-facing HTTP, or WASM-compiled librats client.
  The Dart player on Android + Windows is the only player.
- **VPS colocated mini-node + full node uses 127.0.0.1 fallback** because
  librats's `should_ignore_peer()` blocks dials to any of our local
  interface IPs at any port. Loopback's localhost block is same-port only,
  so `127.0.0.1:<mini-node-port>` from a colocated full node lands on the
  mini-node's accept loop (`src/network/rats_link.cpp:281-298`). This is
  intentional and does not violate [feedback-no-loopback], which forbids
  loopback between *peers* in test setups — colocated services on one box
  are an exception.
- **Phone Doze prompt** — first launch shows the
  `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` system dialog
  (`bopwire_player/android/app/src/main/AndroidManifest.xml:27`). Without
  this, Android's Doze mode pauses the Dart isolate after a few minutes of
  screen-off, the rats client goes idle, and the player drops from every
  swarm. The user has to grant the exemption once per install.
- **20 s discovery refresh + bidirectional `mini.ping`** fills the cellular-
  NAT keepalive gap. LibratsDiscovery wakes every 20 s
  (`bopwire_player/lib/src/services/librats_discovery.dart:62-64`); the
  mini-node's 15 s socket keepalive defends one direction; the player
  ticking discovery defends the other. Without both, a long-idle cellular
  player gets evicted from the CGNAT mapping and looks offline until the
  next user interaction.
- **Multi-VPS topology** per [project-bopwire-multi-vps-topology]. Do not
  model "the VPS" as a single point of failure or trust anchor. Mini-nodes
  mesh; full nodes mesh; players hold a persistent set of known mini-nodes
  in SharedPreferences and rotate through them on watchdog reconnect
  (`bopwire_player/lib/src/services/rats_client.dart:432-449`,
  `:537-567`).
- **Push to GitHub before VPS deploy** per [feedback-push-before-deploy].
  `vps-deploy-all.sh` pulls main and rebuilds; unpushed local changes mean
  home and VPS run different binaries.

---

## 8. Open issues + known limitations

This is the honest list of what isn't fully fixed (updated 2026-06-24 after the de-nerf pass;
items marked ✅ were resolved then — see the internals doc §21 for the full changelog).

1. **✅ FIXED — Per-byte credit via triangulation.** Credit is now `min(relayed,received)`
   bytes × 10 internal units, established by the broker-minted `delivery_id` + the mini's
   signed `relay.report` + the player's signed `relay.receipt` (§5, internals §19.1). A 100 MB
   FLAC earns 10× a 10 MB song. `RelayCreditTracker` accumulates internal units and
   `apply_relay_reward` credits them directly, under the `SUPPLY_CAP` guard. **⚠ New + not
   build-verified; the founder key still signs the `RelayRewardTx` until Phase-3.**
2. **No application-layer ACKs on the binary relay path.** PlayerServer
   paces by time, not by receiver feedback (`player_server.dart:96-97`).
   On a saturated mini-node uplink, chunks are dropped at the F-tag rate
   limiter or queued indefinitely in TCP buffers; the sender doesn't
   notice until a timeout.
3. **✅ FIXED (over-claim) — Signed three-way proof of delivery.** The mini-node can no longer
   over-claim: the broker credits only `min(relayed, received)` where `relayed` comes from the
   mini's signed `relay.report` and `received` from the player's signed `relay.receipt`, both
   bound to the broker-minted `delivery_id` (§5). A lying mini is capped by the player's
   receipt and vice-versa. **⚠ Residual:** a mini+player *colluding* can still inflate up to
   what they jointly sign (bounded by the per-tx unit cap + supply cap); fully closing that
   needs the real device attestation to make Sybil players scarce.
4. **Single-VPS dev/test target.** The codebase supports multi-VPS topology,
   but daily testing uses one box at `85.239.238.226:8080`
   (`rats_client.dart:30`). Mesh edge cases — partition healing, leadership
   between mini-nodes when one goes down mid-stream, route TTL races on
   re-elect — are under-tested.
5. **✅ FIXED — Cross-restart replay protection on `offline.play_proof.submit`.** A
   resubmitted bundle is rejected via a persisted `obp:<sig_hex>` marker (survives restarts).
6. **✅ FIXED — Bot heuristics + structural device attestation.** All 11 heuristics in
   `offline.play_proof.submit` are real (6 hard-rejecting, 4 soft-logging, `obp:hist:<addr>`
   index). The player now ships a **hardware-derived `attestation`** (desktop native FFI
   `mc_device_fingerprint`; Android Kotlin ANDROID_ID + `Build.*`) inside the wallet-signed
   bundle + `session.start`; the `DeviceAttestationVerifier` derives a hardware-bound
   `device_id` and records the level (`dal:` key). *Remaining (needs platform trust roots):
   real battery/BSSID signals + the real hardware-attested verifier (Play Integrity / TPM).*
7. **✅ FIXED — Earning is per-byte, keyed on `delivery_id`, not a verb whitelist.** The relay
   credit is now driven by the F-frame `delivery_id` accounting, so it covers any relayed audio
   bytes the broker brokered — the old single-verb (`stream.open`-only) whitelist is gone.
   `song.audio`/`song.get` were pre-pivot verbs; the live data plane is streams.
8. **F-tag binary relay has no length field.** Receiver-side validation
   trusts librats's framing. A malformed F-tag with a runt body could
   confuse the receiver's stream demuxer; the player's 1 MB hard ceiling
   (`rats_client.dart:1224`) backstops the worst case but not the silent-
   corruption case.
9. **Mini-node chat history is in-memory only.** `kChatRingPerRoom = 1000`
   (`tools/mini_node.cpp:219`) per room, dropped on restart. Multi-mini-node
   sync via gossipsub mesh means a restart eventually re-fills from peers,
   but a fresh mesh-wide restart loses history.
10. **Per-peer rate-limit bucket is per-physical-peer, not per-wallet.** A
    cellular user reconnecting on a new tower gets a fresh peer_id and a
    fresh 50 MB bucket. The pruning in the reaper
    (`tools/mini_node.cpp:1632-1657`) keeps memory bounded but does not
    aggregate abuse across reconnects.

When any of these is fixed, delete the corresponding bullet AND the line in
the section that called it out as a limitation.
