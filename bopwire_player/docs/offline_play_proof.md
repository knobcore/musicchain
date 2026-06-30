# Offline Play-Proof Bundles

Status: design + initial wiring (2026-06-16).

## Threat model

The online play-validation path (`session.start` / `session.heartbeat` /
`session.complete`) assumes the player can talk to the home node every
~5 s for the duration of a song. Listeners on subway tunnels, planes,
patchy cellular, or with wifi/cell both off lose that connection and
their plays drop on the floor — they get no credit and the artist gets
no mint.

Offline-play proofs fix the gap. The player keeps running its own play
clock locally; on reconnect it ships a *play bundle* over the existing
`bopwire.request` rats channel and the home node decides whether to
credit the plays.

The bot vector is the obvious one: if "I played 1000 songs offline" is
trusted blindly, you can synthesize a JSON file and farm tokens. So the
bundle has to carry signals that a sandbox attacker can't easily forge.
We pick four signal groups, each individually weak but mutually
constraining:

1. Heartbeats (same shape as the online verb).
2. Network up/down transitions with BSSID / cell-id.
3. Battery samples.
4. Screen on/off intervals.

## Wire format

Bundle JSON, signed by the player's wallet key. The signature covers
the canonical UTF-8 serialization of the bundle body (everything except
`signature` and `pubkey`).

```jsonc
{
  "bundle_version": 1,
  "player_address": "<40-hex>",       // bundle author
  "pubkey":         "<66-hex>",       // 33-byte compressed, for sig verify
  "bundle_nonce":   "<32-hex>",       // random, replay protection
  "created_at_ms":  1718000000000,    // wall clock at submit time
  "device_id":      "<32-hex>",       // hashed install id, stable
  "monotonic_base_ms": 12345678,      // monotonic clock at bundle start
  "wall_base_ms":      1717900000000, // wall clock at bundle start
  "sessions": [
    {
      "session_id":    "<32-hex>",         // random, unique per (device, song, attempt)
      "content_hash":  "<32-hex>",         // chain content_hash being claimed
      "block_hash":    "<32-hex>",         // optional, 0s if unknown
      "started_wall_ms": 1717900100000,
      "started_monotonic_ms": 12346000,
      "ended_wall_ms":   1717900230000,
      "ended_monotonic_ms": 12476000,
      "song_duration_ms": 240000,
      "heartbeats": [
        { "wall_ms": 1717900100000, "monotonic_ms": 12346000, "position_ms":    0 },
        { "wall_ms": 1717900105000, "monotonic_ms": 12351000, "position_ms": 5000 },
        ...
      ]
    }
  ],
  "network_transitions": [
    { "wall_ms": ..., "monotonic_ms": ..., "kind": "wifi_up",     "fingerprint": "<bssid-or-zero>" },
    { "wall_ms": ..., "monotonic_ms": ..., "kind": "wifi_down",   "fingerprint": "<bssid-or-zero>" },
    { "wall_ms": ..., "monotonic_ms": ..., "kind": "cell_up",     "fingerprint": "<cell-id-or-zero>" },
    { "wall_ms": ..., "monotonic_ms": ..., "kind": "cell_down",   "fingerprint": "<cell-id-or-zero>" }
  ],
  "battery_samples": [
    { "wall_ms": ..., "monotonic_ms": ..., "percent": 78, "charging": false }
  ],
  "screen_intervals": [
    { "on_wall_ms":  1717900090000, "on_monotonic_ms":  12345990,
      "off_wall_ms": 1717900240000, "off_monotonic_ms": 12476000 }
  ],
  "signature": "<128-hex ECDSA>"
}
```

Field notes:

* Heartbeat shape `(session_id, content_hash, position_ms, wall_ms,
  monotonic_ms)` is intentionally a strict superset of the online
  `session.heartbeat` verb so the home node can reuse the existing
  union-of-ranges play-time math in `HttpServer::post_session_complete`.
* `monotonic_ms` is critical for forgery detection: the wall clock is
  user-controllable but the monotonic clock isn't, so the home node can
  cross-check `wall_delta ≈ monotonic_delta` and refuse bundles where
  wall time jumped backwards or where monotonic time went non-
  monotonic.
* `fingerprint` for BSSID is the raw MAC (lowercased, colons stripped)
  and for cell-id is the network-issued tower id. Both are
  network-fingerprint primitives — no SSID name, no neighbor list, no
  GPS, no IMEI, no PII.
* `device_id` is a hashed install id, not a hardware identifier.
  Stable across app restarts so the home node can correlate bundles
  per device, but does not leak hardware.

## Signing key

The bundle is signed by the **player wallet key** (`mc_wallet_sign`,
ECDSA over secp256k1, same key that signs transfer txs). The bundle's
`pubkey` field carries the 33-byte compressed pubkey so the home node
can verify against `player_address`.

We deliberately do NOT use a per-session ephemeral key. The wallet key
is the player's on-chain identity already — reusing it means:

* the home node already has key-recovery infrastructure for ECDSA over
  the wallet's curve,
* a forged bundle from address X has to compromise X's wallet, which
  is the same bar as forging a transfer from X,
* mint outputs already accrue to `player_address`, so the entity
  signing the bundle is exactly the entity being credited.

## Replay protection

* `bundle_nonce` is 32 random bytes. Home node tracks the set of
  used `(player_address, bundle_nonce)` pairs forever (cheap; one
  leveldb prefix lookup).
* Inside each session, the `session_id` is also tracked the same way
  the existing online flow tracks them (`db_.is_session_used`). A bundle
  cannot replay a `session_id` that was previously credited via the
  online verb or a previous offline bundle. We feed the offline session
  ids through the same `mark_session_used` path.
* `created_at_ms` must be within ±24 h of node wall clock — older
  bundles are flat-out rejected to bound the dedup-set memory hit.

## What the node validates

`offline.play_proof.submit` runs these gates in order. Anything that
fails short-circuits the bundle:

1. Signature verify against `pubkey` + `player_address` matches the
   pubkey's derived address.
2. `bundle_nonce` not previously seen for this `player_address`.
3. `created_at_ms` within ±24 h.
4. For each session:
   * `session_id` not used (chain-side dedup).
   * `content_hash` is a known song.
   * `started_wall_ms ≤ heartbeats[*].wall_ms ≤ ended_wall_ms` and
     same for monotonic.
   * Each heartbeat's `monotonic_ms - heartbeats[0].monotonic_ms ≈
     heartbeats[i].wall_ms - heartbeats[0].wall_ms` (±2 s slop) —
     catches synthetic bundles where wall_ms was written by a loop
     and monotonic_ms was just copied.
   * Run the existing union-of-ranges 50%-of-duration listen-time
     check from `post_session_complete`. Sessions that fail this are
     dropped from the bundle (other sessions still credit).
5. Bot heuristics (see next section). Hard fail = reject bundle.

Accepted sessions are then routed into the same `MintTx` pipeline the
online verb uses (`Chain::apply_mint`), so the on-chain reward path is
shared. Royalty splits, discoverer bonus, burn rate, supply caps all
work as-is.

## What the node CAN'T easily validate

Be honest about the limits.

* We can't prove the audio actually played out the speaker. A
  determined attacker with a rooted phone can run the player and
  fake-feed it any signal.
* BSSID and cell-id are observable; an attacker who knows the venue
  layout could replay a believable trace. The signal is best treated
  as one input among many.
* Monotonic clock is per-process. App restart resets it. We accept
  multiple `monotonic_base_ms` epochs across bundles but inside a
  bundle the monotonic timeline must be continuous and forward.
* On iOS, BSSID requires Location and may return zeros for users who
  declined. Bundles with all-zero BSSID still validate but lose that
  forgery signal — the other signals have to carry more weight.

## Bot patterns the heuristics should look for

Stubbed in C++ as named TODOs. Each is a separate function for clarity
and so future contributors can dial individual thresholds without
touching unrelated rules.

1. **PerfectIntervalHeartbeats** — variance of inter-heartbeat
   wall_ms gaps is suspiciously low (e.g. σ < 50 ms over 30+
   heartbeats). Real playback jitters because Dart timers do.
2. **MonotonicClockJumps** — `monotonic_ms` goes backwards within a
   session, or wall/monotonic delta correlation breaks (see "what
   node validates" above; this is the bot-pattern view of the same
   check).
3. **StaticBSSIDLongSession** — a 4 h+ session with cellular fingerprint
   marked "up" the whole time but the cell-id never rotated. Real
   cellular hand off between towers every 10-30 min when stationary
   and every 1-2 min in transit. Same rule for wifi: a single BSSID
   for 6+ hours is plausible (home network) but should boost suspicion
   when combined with other indicators.
4. **BatteryFlatline** — battery_percent never drops or never rises
   across hours of "active" playback. Real phones drop ~2-5 %/hr
   under audio playback.
5. **ScreenAlwaysOn** — screen reported on for the entire bundle
   window. Real users lock their screens between songs. Conversely
   "screen never on" is also suspicious for an offline-listening
   pattern that's supposed to have user interaction.
6. **NoNetworkTransitions** — long offline span (hours) with zero
   wifi/cell transitions logged. A real device sees radio state
   changes constantly (cell handoff, wifi roaming, sleep wake).
7. **HeartbeatDensityTooHigh** — submitted bundle has >12 heartbeats
   per song-second, indicating someone over-densified to game the
   union-of-ranges integration.
8. **ImplausibleSessionConcurrency** — overlapping sessions for
   different `content_hash`es on the same device. A phone plays one
   song at a time. Multiple in flight means script.
9. **DeviceIDChurn** — same `player_address` ships bundles with many
   different `device_id`s in a short window. Real users have 1-3
   devices, not 50.
10. **WalletAgeVsPlayVolume** — fresh wallet (recent first transfer
    or no on-chain history) submitting hundreds of plays per day is a
    common farming signature.

The first cut implements these as TODO-stubs that always pass; the
verb still goes through the bundle and credits sessions. A follow-up
pass actually computes each metric and emits a `rejected_reasons[]`
array in the reply so the player UI can show the listener why their
bundle was throttled and the home node logs every flagged pattern for
post-hoc tuning.

## Client-side persistence

* `HeartbeatCapture` writes to a sqflite table on the same db as the
  cache (one new table; cheap migration). Survives app crash, OS
  kill, reinstall? Sqflite persists across crash and kill; reinstall
  wipes app data so unsubmitted bundles are lost. That's an acceptable
  trade: anyone reinstalling is likely a real user, not a farmer.
* Submission is best-effort. On a successful POST the rows are
  marked submitted; on partial-success (some sessions rejected) the
  accepted ones are marked submitted and the rejected reasons are
  surfaced in a notification but not retried.
* Maximum buffer: 7 days, 5000 sessions, 50 MB of rows. Beyond that
  we drop the oldest rather than crash.

## Open follow-ups

* Cell-id capture on Android requires a method-channel into
  `TelephonyManager.getAllCellInfo()`. Stubbed as zero-fingerprint
  for now; design the channel interface but don't ship it in this
  pass.
* Cell-id on iOS is straight-up not exposed; bundles from iOS will
  always have zero cellular fingerprints. Document this in user-
  facing copy.
* Screen on/off uses `AppLifecycleState` as a proxy; the OS lock-screen
  doesn't always fire it. A platform channel listening to
  `Intent.ACTION_SCREEN_OFF` on Android will be more accurate; iOS
  needs `UIApplication.protectedDataDidBecomeUnavailable`.
* Battery uses `BatteryManager` on Android and `UIDevice` on iOS via
  a platform channel — see `SensorCapture._readBatteryPercentStub`
  for the call site once a plugin lands.
* Full bot-detection thresholds need tuning against real bundle
  traffic; current stubs are intentionally inert.
