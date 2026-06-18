# mcCOIN on Base — bridge design v1 (merkle-drop)

Custodial wrapped-token bridge that puts a wrapped representation of
musicchain's native coin on Base (Coinbase's L2). The deposit path is
a **merkle distributor** so the founder's gas bill stops scaling with
playback volume — a design that survives Spotify-scale (~8 B
streams/day) where naive per-play minting would burn ~$240M/day on
Base or ~$24M/day on NEAR.

## What v1 changes vs v0 (per-play mint)

| Question | v0 (per-play mint) | v1 (merkle-drop, THIS DOC) |
| --- | --- | --- |
| Who mints on Base? | Daemon, once per user deposit | Users, when they choose to |
| Who pays gas? | Founder, scales with volume | User, paid out of pocket per claim |
| Daemon ops at 8 B plays/day | $24M – $240M/day, impossible | ~$10/day, flat |
| Daily on-chain ops | Millions of `mint()` calls | One `setRoot()` per day |
| Storage footprint on Base | One ERC-20 transfer per play | One root per day, plus claim history |
| Trust assumption | Trust daemon to mint correctly | Trust daemon to publish honest root; tree is publicly auditable |
| Withdraw side (burn → native mc) | Same in both | Same in both |

## Why this shape

| Pattern | Trust | Build cost | TVL ceiling |
| --- | --- | --- | --- |
| **Custodial wrapped token** (this doc) | Trust founder | days | low–mid |
| Hyperlane warp route | Trust Hyperlane validators | weeks | mid |
| Canonical OP-stack bridge | Trustless (7-day exit) | months | high |

musicchain's social contract already trusts the founder (Phase 2
moderator system, escrow release, etc.) so the custodial pattern
re-uses an assumption that already exists. When TVL outgrows the
founder-key risk model, swap to Hyperlane warp routes (same ERC-20
contract, different bridge owner).

## Architecture

```
musicchain (native)         founder bridge daemon         Base (L2)

  user wallet                                              user wallet
      |                                                         |
      |                     daily root publish                  |
      |                     ─────────────────                   |
      |                     1. snapshot every player's          |
      |                        cumulative bridgeable mc balance |
      |                     2. derive Base address from each    |
      |                        recovered pubkey                 |
      |                     3. build merkle tree, publish raw   |
      |                        leaves to IPFS                   |
      |                     4. sign + send ONE tx ─────────►    mcCOIN.setRoot(
      |                                                            root,
      |                                                            ipfs://…/leaves.json)
      |                                                              │
      |                                                              ▼
      |                                                       RootUpdated event
      |                                                              │
      |                     user-driven claim                        ▼
      |                     ─────────────────                user pulls proof from
      |                                                       data URI, calls:
      |                                                  mcCOIN.claim(cumAmt, proof)
      |                                                              │
      |                                                              ▼
      |                                                       user holds mcCOIN
      |
      |                       withdraw loop
      |                       watcher polls Base for BridgeBurn events
      |                            ^
      |                            │
      |                       mcCOIN.burn(amount, mcAddress) <-- user calls
      |                            │
      |                       waits for L2 finality
      |                            │
      |  TRANSFER  <-- signs ──────┘
      |  amount=N / 1e10
      |  from=bridge_addr
      |  to=mcAddress
      |
  user wallet
```

### Key shift: no per-deposit minting

The deposit ledger lives on musicchain, not on Base. Native mc never
leaves musicchain. The daemon's only on-chain action on the Base side
is publishing one merkle root every 24 hours. When the user wants to
hold mcCOIN (to trade, transfer to MetaMask, sit on it), they call
`claim()` themselves and pay their own gas. Users who never claim
generate zero ongoing cost.

### Cumulative balances, not deltas

Each new root replaces the previous one and carries **cumulative**
balances per user. A user can claim a snapshot once a year if they
want — the contract tracks `claimed[user]` and only mints the delta.
This means:

* Old roots don't need to stay valid — only the latest matters
* Users don't have to claim every epoch to avoid losing rewards
* The merkle tree size grows with total active users, not with
  cumulative playback

### Leaf format

Each leaf encodes `(base_address, cumulative_bridgeable_amount)` using
the OpenZeppelin double-hash format that `MerkleProof.verify` expects:

```
leaf = keccak256(bytes.concat(keccak256(abi.encode(user, cumulativeAmount))))
```

`user` is the 20-byte Base address derived from the same secp256k1 key
the player already uses for musicchain (via `eth_address_from_pubkey`
on the C++ side). For users who haven't yet signed any musicchain
transaction (so the daemon hasn't recovered their pubkey), they're
omitted from the tree until they do; their unclaimable balance just
sits on musicchain until the next epoch picks them up.

## Decimal scaling

- musicchain native: 8 decimals (`internal_units = mc * 1e8`).
- Base / Ethereum convention: 18 decimals.
- Bridge daemon scales on both sides: `mint_amount_base = deposit_mc_internal * 1e10`,
  `withdraw_mc_internal = burn_amount_base / 1e10`. Burn amounts that
  aren't a multiple of `1e10` round down and the dust stays in the
  contract; the daemon refuses to act on burns that round to zero.

## Components

### 1. mcCOIN ERC-20 contract — `contracts/mcCOIN.sol`

ERC-20 with the following non-standard surface:

- `setRoot(bytes32 root, string dataURI)` — bridge owner only. Stores
  the new root + bumps `currentEpoch` + records the IPFS / HTTPS URI
  of the raw leaf data. Emits `RootUpdated`.
- `claim(uint256 cumulativeAmount, bytes32[] proof)` — anyone with a
  valid proof against `currentRoot`. Mints `cumulativeAmount -
  claimed[msg.sender]` to the caller. Emits `Claimed`. Cap-checked.
- `burn(uint256 amount, bytes20 mcAddress)` — anyone holding mcCOIN.
  Emits `BridgeBurn(from, amount, mcAddress)` so the daemon knows
  which musicchain address to credit.
- `previewClaim(address user, uint256 cumulativeAmount) view` — UI
  helper returning the delta a claim would mint.
- `pause()` / `unpause()` — owner kill-switch.
- `cap` — hard ceiling matching `SUPPLY_CAP` scaled to 18 decimals.

Notably absent vs v0: no `mint()`, no `mintCapPerWindow`. Per-window
rate limit is irrelevant when the only owner-callable action is
`setRoot()`, which doesn't itself mint anything (users do the minting
via claims). A forged root is bounded by the global `cap` and caught
by the off-chain tree-sum audit.

OpenZeppelin v5 base: `ERC20`, `Ownable2Step`, `Pausable`,
`MerkleProof`.

### 2. Bridge daemon — `tools/base_bridge/`

Language: **Go** (matches musicchain's operational tooling — librats is
C++, the chain is C++, daemon glue should be a small statically-linked
binary with easy systemd deploy). Alternative: TypeScript via viem.

Modules:

| File | Purpose |
| --- | --- |
| `cmd/bridge-base/main.go` | Service entrypoint; reads config; runs root-publisher + burn-watcher in parallel. |
| `snapshot.go` | Once per epoch (default: every 24h): walks musicchain's balance table, recovers each account's secp256k1 pubkey from its most recent signed tx, derives the Base address via keccak256, and builds the per-user `cumulativeBridgeable` map. Skips accounts the daemon hasn't seen a signed tx from yet. |
| `merkle.go` | Builds the OpenZeppelin-format tree (double-hashed leaves), produces per-user proofs alongside the root. |
| `publisher.go` | Pins the raw leaves to IPFS (or uploads to a public S3 bucket with content-hash naming), then signs + broadcasts `mcCOIN.setRoot(root, dataURI)` from the Safe delegate key. |
| `base_watcher.go` | Subscribes to `BridgeBurn` events on the mcCOIN contract via WebSocket; queues the withdraw side. |
| `mc_sender.go` | Builds + signs musicchain TRANSFER txs from `cfg.MusicchainBridgePriv`, draining native mc back to burners. |
| `state.go` | SQLite: epoch checkpoint, last-published root, withdraw dedup keyed on `base_burn_tx_hash`. |

Deposit-side replay protection is **on chain** via the cumulative
tracking (`claimed[user]` and the cap check), so the daemon doesn't
need a deposit-side dedup table at all — re-publishing the same root
is idempotent, and a forged or duplicate setRoot just sets a state
that users can't over-claim against.

Withdraw side still needs SQLite dedup so the daemon doesn't double-
credit native mc on a restart mid-flight: write the row **before**
broadcasting the TRANSFER, update with the resulting tx hash after.
On restart, replay any "broadcasted but not confirmed" rows.

### 3. Founder ownership

- The mcCOIN contract owner should be a **Gnosis Safe** on Base, not
  the founder's EOA. The daemon's hot key is a delegate of the Safe
  with a low spending allowance; high-impact actions (pause,
  setMintLimit, transferOwnership) require the Safe's full m-of-n
  signers.
- The Safe protects against: compromised daemon host, founder laptop
  exfiltration, single-key social-engineering. It does NOT protect
  against social engineering of the Safe signers themselves — keep the
  signer set tight (e.g. 2-of-3 across cold devices the founder
  controls + one community signer).

### 4. Player UI

Two new screens under `musicchain_player/lib/src/screens/`:

- `bridge_claim_screen.dart` — fetches the user's row from
  `currentDataURI`, displays "Y mcCOIN ready to claim", lets them sign
  the `claim()` tx via the in-app Base signer (same secp256k1 key
  they already have). Auto-derives the proof; user just taps "Claim".
  Shows "nothing to claim" when `cumulativeAmount <= claimed[user]`.
- `bridge_withdraw_screen.dart` — user enters amount; player builds +
  signs the `mcCOIN.burn(amount, mcAddress)` call directly via the
  in-app Base signer. mcAddress is derived automatically from the
  same key (= `sha256(compressed_pubkey)[..20]`), so the user never
  types an address.

Both screens use the same Base signer, which is a thin Dart-side ECDSA
+ RLP module bound to the existing `mc_wallet_*` C-API exports. No
MetaMask install required; the player wallet IS the Base wallet.

Per-screen daemon dependencies:

- Claim screen depends on the daemon publishing a non-stale root. The
  player polls the contract's `currentEpoch` and warns if the latest
  on-chain epoch is older than `MAX_ROOT_AGE` (default: 36h).
- Withdraw screen has no daemon dependency at the signing moment — the
  burn lands on chain immediately. The daemon services the
  burn-to-mc credit asynchronously (typically <30 min).

## Security posture

| Risk | Mitigation |
| --- | --- |
| Daemon hot-key compromise → unbacked mint | Daemon's signer is a Safe delegate with a per-window allowance; large mints require additional Safe signatures. |
| Daemon downtime → user can't withdraw / redeem | Daemon is stateless beyond SQLite; restart resumes. Pause contract if expected outage > 24h. |
| Base sequencer reorg → daemon credits mc but Burn event reverts | Wait `confirmations >= 64` on L1 batch posting (≈ 10 min in 2026) before releasing mc. |
| Musicchain reorg → daemon mints on Base but the deposit reverts | Wait 6 musicchain blocks (= ~30 min at 5-min cadence) before mint. |
| Decimal-conversion rounding dust | Burn amounts that round to 0 mc are reverted at the contract level; non-zero rounding stays as locked collateral and reduces effective bridge ratio over time (negligible at typical scales). |
| Bridge address private-key loss → locked collateral becomes unrecoverable | Bridge wallet is itself a multi-sig on the musicchain side once Phase 5+ ships HD wallets; until then, founder holds two hot/cold copies and rotates quarterly. |
| Coinbase listing rejection (legal) | Submit Howey-test self-assessment early; track FinCEN guidance; consult counsel before > $1M market cap. |

## Deployment phases

1. **Anvil round-trip** — deploy mcCOIN to a local Anvil node; bridge
   daemon runs against a local musicchain node + Anvil; mint and burn
   one round-trip. Pure smoke test.
2. **Base Sepolia** — same flow against testnet Base + a testnet
   musicchain instance. Faucet-funded; verify Gnosis Safe ownership
   transfer; rate-limit testing.
3. **Base mainnet contract deploy** — Safe ownership at deploy time,
   not transferred-to afterward.
4. **Liquidity seeding** — founder deposits ~10k mc on musicchain →
   daemon mints ~10k mcCOIN on Base → founder pairs with ETH on
   Aerodrome at the spot price they choose (Aerodrome > Uniswap on
   Base for early liquidity).
5. **CoinGecko + CMC submission** — fill out their forms, include the
   bridge audit trail (BridgeMint events on basescan all link back to
   musicchain tx hashes that are independently verifiable).
6. **Monitor** — daemon dashboards (Grafana on the VPS host), reserve
   ratio alerts, BridgeMint:BridgeBurn rate alerts.
7. **Coinbase Custody / listing review** — apply when 30-day volume
   stabilizes above their listing threshold (currently
   ~$100k/day liquidity at $1M+ market cap).

## Security non-goals — things this bridge will explicitly NOT do

This is an enforcement list. Every line is a foot-gun a peer bridge
shipped and got drained on, restated here as "we will not." When the
implementation diverges from any of these, we either (a) revert to
the rule, or (b) write a paragraph in this section explaining why the
threat doesn't apply to our setup. Drift without paperwork = bug.

### 1. No cross-chain signature replay

Same secp256k1 key, two domain-separated sign-message shapes. The
chain's `TransferTx::sign_message()` and Ethereum's RLP-encoded tx
with EIP-155 `chain_id` cannot collide. We will NOT add a generic
"sign this hash" verb on either side that would let one chain's
signature be replayed against the other.

### 2. No reliance on daemon state for replay protection

The mcCOIN contract carries a `depositMinted[bytes32]` mapping; a
mcDepositTxHash that has been minted once reverts on the second
attempt regardless of whether the daemon's SQLite forgot. We will
NOT trust the daemon's dedup table alone — every replay defense is
on chain.

### 3. No unbounded mint authority

In v1 (merkle-drop) the contract carries `cap` (global supply
ceiling) as the only on-chain mint bound, because the owner doesn't
mint directly — users do, against a root the owner publishes. The
defence-in-depth is the off-chain **tree-sum monitor** that verifies
`sum(leaves[currentRoot]) == musicchain.bridgeable_native_mc` every
epoch and triggers `pause()` on discrepancy. We will NOT deploy
without that monitor running on independent infrastructure (different
cloud + alerting channels from the daemon itself).

### 4. No releasing native mc before L1 finality

The daemon waits for `confirmations >= 64` blocks on the L1 batch
that includes a `BridgeBurn` event before broadcasting the matching
native-mc TRANSFER (~10 min at 2026 Base settlement cadence). We
will NOT release mc on bare L2 confirmation alone — Base sequencer
re-orgs do happen.

### 5. No mint-before-musicchain-finality

Same argument the other direction: daemon waits 6 musicchain blocks
(~30 min at the 5-min cadence) after a deposit TRANSFER lands
before signing the `mint()` call. We will NOT shortcut this for
"fast bridge" UX.

### 6. No flexible bridge address discovery

The musicchain bridge address is hardcoded in:
  * the player binary
  * the home-node binary (used by the explicit `bridge.lock` verb)
  * docs/base-bridge.md (this file)

We will NOT let the RPC, the chain, or a config file override it.
A compromised RPC swapping the bridge address is a one-line phish
that drains every deposit forever.

### 7. No founder-controlled liquidity rug

When the founder seeds the Aerodrome / Uniswap pool, the LP tokens
get time-locked in a third-party time-lock contract (Sablier or
similar) with an unlock schedule visible on basescan. We will NOT
deploy with the founder holding unlocked LP tokens. The lock period
follows the standard CEX listing cadence (12-month linear vest).

### 8. No reentrancy hooks on burn

`McCoin.burn(amount, mcAddress)` calls `_burn(msg.sender, amount)`
which has no callback. We will NOT add ERC-777 `tokensToSend` hooks,
ERC-1363 `transferAndCall`, or any pattern that yields control to
the burner mid-operation. If a future integration needs a "burn
notification" it gets its own wrapper contract, not a hook in the
core.

### 9. No on-chain governance

The contract has no `vote()`, no `Governor`, no token-weighted
upgrade authority. Owner = Safe = fixed signer set; ownership
transfer is `Ownable2Step` so it requires explicit signing on both
sides. We will NOT add governance — flash-loan-vote attacks have
drained too many contracts that did.

### 10. No public mempool for the initial liquidity seed

Founder's first `addLiquidity` call goes through Flashbots / private
relay on Base. The whole deposit-and-LP-seed transaction is one
atomic bundle. We will NOT expose the initial price discovery to
public-mempool MEV bots; the founder's first swap is the only one
big enough to sandwich-meaningfully and we eat the relay cost to
avoid it.

### Bonus — what we DON'T need to defend against on this layout

Because the daemon mint is `onlyOwner`-gated, there's no "anyone can
mint with a proof" surface — so we don't need cross-chain inclusion
proofs, no merkle bridge, no fraud-window dispute game. The trust
model collapses to "trust the founder Safe", which is the same
assumption every other moderator action already lives under. If
that assumption falls apart, mcCOIN is the least of our problems.

## Open questions

- **Bridge address derivation.** Generated wallet (write the priv key
  to the Safe) or deterministic from founder key (HKDF salt
  "mc-bridge-v1")? Deterministic is operationally simpler;
  generated-with-rotation is more secure.
- **Base RPC provider.** Alchemy / QuickNode / Infura / self-hosted
  Reth? Start with Alchemy (free tier covers 100k req/day) and switch
  when daemon load demands it.
- **Audit budget.** Solidity firms (Spearbit, Cantina) charge $30k+
  for a one-week sweep. Justifiable above ~$500k TVL; before that the
  contract is simple enough for an OpenZeppelin-only review +
  formal Echidna fuzzing.
- **Why not just deploy on Solana / native EVM rollup of our own?**
  Coinbase Wallet integration is the goal, and Coinbase already
  prefers Base assets for Asset Discovery / Earn / Pay. Solana would
  duplicate the bridge work for a smaller listing payoff.

## Files this lands

- `contracts/mcCOIN.sol` — the ERC-20 above. Sketch attached as a
  sibling commit; not yet deployed anywhere.
- `tools/base_bridge/` — Go skeleton (cmd, mc_watcher, etc.). Not yet
  implemented in this design pass.
- `docs/base-bridge.md` — this file.
- `musicchain_player/lib/src/screens/bridge_*_screen.dart` — player
  UI; deferred until daemon is alive on Base Sepolia.
