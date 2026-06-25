# MusicChain — Blockchain & Network Stack Internals

> **Scope.** This document describes, step by step, how the MusicChain blockchain
> validates state and how the network stack moves blocks, transactions, and audio
> between nodes — *as the code on disk actually behaves today*, not as the design
> aspires to. It is the validation-and-internals companion to
> [`ARCHITECTURE.md`](../ARCHITECTURE.md) (which covers node roles, topology, and the
> relay/NAT model). Where the two overlap, this file goes deeper on the chain and
> consensus mechanics and assumes you have skimmed the topology overview.
>
> **Reading convention.** Every claim is anchored to `file:line`. When prose and code
> disagree, code wins. Sections flagged **⚠ NERFED** describe machinery that is present
> but inert, stubbed, or self-defeating in the current build — these are the spots the
> architecture is "kinda nerfed," called out explicitly so you can decide what to
> restore.
>
> Generated 2026-06-24 against `main` @ `dbe760f`.

---

> ## ⚙ Implementation status — Model 1 landed (uncommitted working tree)
>
> A first implementation pass converted the chain to **Model 1 (vote-free
> deterministic consensus)** and fixed the mechanical nerfed items. Sections 1–20 below
> still describe the *pre-change* behavior in places; this banner and the per-item notes
> in §21/§22 are the source of truth for what changed. **Done in code:**
>
> - **Block format → v3.** `BlockHeader.confirmations` and `signing_hash()` removed;
>   `BLOCK_VERSION` bumped 2 → 3 ([`block.h`](../src/core/block.h), [`block.cpp`](../src/core/block.cpp)).
>   `block.hash()` is now the single canonical hash. **⚠ Consensus-breaking: existing
>   v2 chains/DBs won't deserialize and must be wiped** (`scripts/wipe-and-run-home.ps1`).
> - **Vote-free consensus.** `BlockCandidate` / `dynamic_quorum` / `MAX_CONFIRMATIONS` /
>   `add_confirmation` and the confirmation wait removed; `CandidateManager::commit_block`
>   is now build → `connect_block` → announce ([`candidate.cpp`](../src/consensus/candidate.cpp)).
> - **No candidate/confirmation gossip.** `mc:block_candidate` / `mc:block_confirmation`
>   channels + handlers/trampolines removed from [`rats_link`](../src/network/rats_link.cpp),
>   [`node_main`](../tools/node_main.cpp), and [`manager.h`](../src/network/manager.h). Block
>   distribution is BlockPropagator only.
> - **Deterministic ingest/replay.** Block-level confirmation checks removed from
>   [`block_propagator.cpp`](../src/sync/block_propagator.cpp) ingest and
>   [`chain.cpp`](../src/core/chain.cpp) replay (tx signatures still verified).
> - **Transfers fixed (#1).** `TransferTx` carries an inline `from_pubkey`; `verify_signature`
>   cross-checks the address and calls `verify_ecdsa` ([`transaction.cpp`](../src/core/transaction.cpp),
>   `post_transfer` in [`server.cpp`](../src/api/server.cpp)). The always-false
>   `verify_ecdsa_from_address` path is no longer used by any live code.
> - **Relay-reward supply cap (#6).** `apply_relay_reward` rejects mints that would exceed
>   `SUPPLY_CAP` ([`chain.cpp`](../src/core/chain.cpp)).
> - **Legacy dead code deleted (#9).** `ValidatorRegistry`, the libuv mesh (`peer`, `messages`),
>   and `HttpGossip` (`registry_announcer`) removed; libuv + libcurl dropped from CMake.
> - **Deep Auditor teeth + mesh gossip (#4).** A forgery → local `mark_song_deleted` AND a
>   node-signed `forgery_report` over the mod-log; receivers corroborate by K=2 reporters or
>   re-audit before deleting ([`deep_audit.cpp`](../src/sync/deep_audit.cpp), [`rats_api.cpp`](../src/api/rats_api.cpp)).
> - **Offline-proof anti-abuse (#5).** `obp:<sig>` replay protection + all **11 bot heuristics**
>   (6 hard / 4 soft, `obp:hist:` index) + a pluggable `DeviceAttestationVerifier`
>   ([`device_attestation.h`](../src/api/device_attestation.h)).
> - **Config-driven checkpoints (#7).** `Chain` consults checkpoints from `config.json` in
>   connect/reorg/replay ([`chain.cpp`](../src/core/chain.cpp)); list still empty pending an audited height.
> - **Demand-weighted fork choice + automatic reorg (#8).** Fork weight = cumulative audited
>   plays (`MintTx` count); `cw:` index + `ChainTip.weight`; `Chain::reorg_to_branch`
>   (try-and-rollback, unified on `tip_is_better`, full-adoption-or-restore) wired into the
>   propagator ([`chain.cpp`](../src/core/chain.cpp), [`block_propagator.cpp`](../src/sync/block_propagator.cpp)).
> - **Base bridge deleted.** `mcCOIN.sol` + `base-bridge.md` removed — **MC is internal-only**,
>   which severs the Axis-B fork-to-cash amplifier (§22.6).
> - **Relay-reward triangulation (#10/#6) — IMPLEMENTED (coordinated 3-binary change).**
>   The broker mints a `delivery_id` at `stream.open` (`pd:` row); the mini-node stamps it
>   into the relay F-frame, accounts bytes, and broadcasts a signed `relay.report`; the
>   player sends a signed `relay.receipt`; the broker credits `min(relayed,received)` bytes
>   × 10 internal units (1 MC / 10 MB) on three-way corroboration, single-use
>   ([`rats_api.cpp`](../src/api/rats_api.cpp), [`mini_node.cpp`](../tools/mini_node.cpp),
>   player `rats_client.dart`/`player_server.dart`). See §19.1.
> - **Structural device attestation (#5) — IMPLEMENTED (interface + record tier).** Desktop
>   ships a native hardware fingerprint (MAC + board/CPU/disk + OS + MachineGuid via FFI
>   [`hw_fingerprint.cpp`](../src/util/hw_fingerprint.cpp)); Android a Kotlin one (ANDROID_ID
>   + Build.*); both ride INSIDE the wallet-signed offline bundle + `session.start` as an
>   `attestation` object. The full node's `AcceptAllVerifier` derives `device_id` from it,
>   **records the level** (`dal:` key), and accepts — a real hardware verifier swaps in with
>   no client change (§22.6).
>
> **Still deferred (⚠):** the *real hardware-attested* verifier (Play Integrity / DeviceCheck /
> TPM — the structural tier above is the seam it drops into) + an online-session per-device
> concurrency cap (§22.6). Two adversarial review passes (C++ + Dart) were run + findings
> fixed, but **none of this is build-verified** in the authoring environment — compile and
> iterate. The reorg path and the #10 relay flow in particular are new and have no automated
> test yet.

---

## Table of contents

1. [System shape in one page](#1-system-shape-in-one-page)
2. [Cryptographic primitives](#2-cryptographic-primitives)
3. [The block data model](#3-the-block-data-model)
4. [Transactions](#4-transactions)
5. [Merkle root](#5-merkle-root)
6. [The chain: block acceptance & state transition](#6-the-chain-block-acceptance--state-transition)
7. [Per-transaction-type apply rules](#7-per-transaction-type-apply-rules)
8. [Token economics & the ledger](#8-token-economics--the-ledger)
9. [Consensus: candidates, quorum, confirmations](#9-consensus-candidates-quorum-confirmations)
10. [Block production loop & timing](#10-block-production-loop--timing)
11. [Slashing](#11-slashing)
12. [Storage: the LevelDB schema](#12-storage-the-leveldb-schema)
13. [Network transport: librats wrapper](#13-network-transport-librats-wrapper)
14. [Block propagation protocol](#14-block-propagation-protocol)
15. [Chain replay & startup validation](#15-chain-replay--startup-validation)
16. [Deep audit (anti-forgery)](#16-deep-audit-anti-forgery)
17. [RPC API surface & chain-affecting verbs](#17-rpc-api-surface--chain-affecting-verbs)
18. [Swarm index (content availability)](#18-swarm-index-content-availability)
19. [Relay-credit economy](#19-relay-credit-economy)
20. [Fork choice, checkpoints, reorg](#20-fork-choice-checkpoints-reorg)
21. [Consolidated "nerfed" inventory](#21-consolidated-nerfed-inventory)
22. [Chosen architecture: proof-of-unique-song (vote-free deterministic consensus)](#22-chosen-architecture-proof-of-unique-song-vote-free-deterministic-consensus)

---

## 1. System shape in one page

MusicChain is an **account-based** (not UTXO) blockchain whose primary on-chain asset
is a song registration: a fingerprint + content hash + metadata. The token (MC) layer
rewards plays, discovery, and relay traffic. The chain is deliberately
**Ethereum/EVM-address-compatible** so external wallets derive the same 20-byte
address from the same key.

Three binaries, three jobs (full detail in [`ARCHITECTURE.md` §1](../ARCHITECTURE.md)):

| Role | Binary | Runs chain code? | Holds audio? |
|---|---|---|---|
| **Full node** | `musicchain-node` | Yes — `mc::Chain` + `BlockPropagator` + `CandidateManager` | Content-addressed file store |
| **Mini-node** | `musicchain-mini-node` | No — pure relay/router | No |
| **Player** | Android + Windows (Dart) | No — RPC client + swarm peer | Local downloads only |

The chain lives only on full nodes. Everything in sections 3–16 of this document runs
inside `musicchain-node`. The transport (section 13) is **librats**, a vendored QUIC/TCP
P2P library frozen at v0.2.0; all RPC verbs ride on top of it as typed JSON messages.

A single full node, started cold, will:

1. Open LevelDB at `<data_dir>/blockchain.db` and load the tip (`Chain::init` →
   `Chain::load_tip`, [`chain.cpp:21-43`](../src/core/chain.cpp#L21)).
2. Start librats, find a mini-node, subscribe to the routes topic
   (`RatsLink::start`, [`rats_link.cpp:37-108`](../src/network/rats_link.cpp#L37)).
3. Start `BlockPropagator` (block sync) and `CandidateManager` (block production).
4. If it is the very first node, mint the **genesis block** via the solo self-sign
   fast path and become the chain authority.

Each of those steps is dissected below.

---

## 2. Cryptographic primitives

All crypto lives under `src/crypto/` in namespace `mc::crypto`, built on **OpenSSL**
(EC/ECDSA/AES-GCM/SHA-256/HKDF/PBKDF2), **libwally-core** (BIP39/BIP32), an optional
**libsodium** (Argon2id), and a **self-contained Keccak-256**.

Fixed-width types ([`block.h:37-40`](../src/core/block.h#L37)):

```cpp
using Hash256  = std::array<uint8_t, 32>;
using Address  = std::array<uint8_t, 20>;
using PubKey33 = std::array<uint8_t, 33>;   // compressed SEC1 point
using Sig64    = std::array<uint8_t, 64>;   // compact r||s, NO recovery byte
```

### 2.1 Curve & keys

- **Curve:** secp256k1 everywhere — `EC_KEY_new_by_curve_name(NID_secp256k1)`
  ([`keys.cpp:37-41`](../src/crypto/keys.cpp#L37)).
- **Private key:** 32-byte scalar, left-zero-padded ([`keys.cpp:29-35`](../src/crypto/keys.cpp#L29)).
- **Public key:** 33-byte compressed point (`POINT_CONVERSION_COMPRESSED`,
  [`keys.cpp:52-54`](../src/crypto/keys.cpp#L52)).
- **`keypair_from_priv_bytes(priv32)`** ([`keys.cpp:77-95`](../src/crypto/keys.cpp#L77)) is the
  *single supported* way to turn 32 raw bytes into a keypair. It treats the bytes as the
  raw scalar with **no rehash** — this invariant is load-bearing: rehashing would shift
  the derived address off what MetaMask/ethers.js produce. The header comment hammers
  this point ([`keys.h:18-34`](../src/crypto/keys.h#L18)).

### 2.2 Address derivation (Ethereum-identical)

`address_from_pubkey` ([`keys.cpp:117-132`](../src/crypto/keys.cpp#L117)):

1. Decompress the 33-byte pubkey to the EC point.
2. Re-serialize **uncompressed** → 65 bytes `0x04 || X || Y`.
3. `keccak256` over the 64 bytes `X||Y` (skip the `0x04` prefix).
4. Take the **last 20 bytes**.

This is exactly Ethereum's `keccak256(pubkey[1..65])[12..32]`. EIP-55 checksum casing is
in `to_checksum_hex` ([`keys.cpp:148-173`](../src/crypto/keys.cpp#L148)); mixed-case input is
validated against the recomputed checksum in `parse_address_checksummed`
([`keys.cpp:181-214`](../src/crypto/keys.cpp#L181)).

**Escrow addresses** are keyless and deterministic:
`escrow_address_for(artist) = sha256("escrow:" || artist)[0..20]`
([`hash.cpp:84-97`](../src/crypto/hash.cpp#L84)). No secp256k1 key maps to them — only the
moderator `RELEASE_ESCROW` path can drain them.

### 2.3 Hashes

- **SHA-256** (OpenSSL): used for transaction signing pre-hash, block hashing, merkle,
  escrow derivation, BIP39 checksum, and session/replay keys.
- **Keccak-256** (self-contained, *not* NIST SHA-3 — padding byte `0x01` vs `0x06`,
  [`keccak256.h:8-23`](../src/crypto/keccak256.h#L8)): used *only* for address derivation and
  EIP-55 casing. There is also a byte-identical C port `keccak256_c.c` for the NDK bridge.

Two hash domains coexist: **keccak for addresses, SHA-256 for signing.** Anyone signing a
MusicChain tx must SHA-256 the preimage, not keccak it.

### 2.4 Signatures — and the recovery gap ⚠ NERFED

Scheme: ECDSA/secp256k1, **64-byte compact `r||s`**, big-endian, **no DER, no `v`
recovery byte**.

```cpp
Sig64 sign_ecdsa(const Hash256& hash, const std::vector<uint8_t>& priv);   // signature.cpp:64
bool  verify_ecdsa(const Hash256& hash, const Sig64& sig, const PubKey33& pubkey); // :78
bool  verify_ecdsa_from_address(const Hash256& hash, const Sig64& sig, const Address&); // :88
```

`sign_ecdsa` signs the already-computed 32-byte hash directly ([`signature.cpp:64-76`](../src/crypto/signature.cpp#L64));
`verify_ecdsa` rebuilds the key from the 33-byte pubkey and calls `ECDSA_do_verify`
([`signature.cpp:78-86`](../src/crypto/signature.cpp#L78)). These two work.

**`verify_ecdsa_from_address` is a hardcoded `return false`**
([`signature.cpp:88-96`](../src/crypto/signature.cpp#L88)):

```cpp
bool verify_ecdsa_from_address(...) {
    // ECDSA does not support key recovery with basic OpenSSL ...
    // placeholder that always fails ...
    return false;   // TODO: implement ECDSA recovery with libsecp256k1
}
```

Consequence: **any code path that verifies a signature against only an address (without
an inline pubkey) cannot succeed.** This recovery gap is genuinely still open. But it no
longer blocks transfers: `TransferTx` was reworked to carry `from_pubkey` inline and verify
via `verify_ecdsa`, so on-chain MC transfers now work (see §4.2, §7.1, §21 #1) and do not
touch the address-only path. Every other transaction type already carries its signer's
pubkey inline and uses `verify_ecdsa`, so they are unaffected too.

Two further caveats: there is **no low-s / malleability normalization** anywhere (OpenSSL
accepts both `s` and `n−s`), and `sign_data`/`verify_data` use **SHA-256**, so this is
not an EIP-191/`personal_sign` signer despite the EVM-style addresses.

### 2.5 ECIES (moderator inbox)

Multi-recipient ECIES on secp256k1 + AES-256-GCM, HKDF-SHA256 key wrap
([`ecies.cpp`](../src/crypto/ecies.cpp)). One random 32-byte content key encrypts the body once;
for each recipient a fresh ephemeral keypair does ECDH → HKDF(`info="mc-ecies-v1"`,
salt = recipient address) → AES-GCM-wraps the content key. Wire format `MCE1 || count ||
plaintext_len || [113-byte slots] || body_nonce || body_ct || body_tag`
([`ecies.h:18-33`](../src/crypto/ecies.h#L18)). Used to encrypt DMCA/KYC submissions to the union
of active moderators so any one can decrypt with their key. Recipient unlinkability is
explicitly *not* a goal (addresses appear in the clear in each slot).

### 2.6 BIP39 / wallet derivation

The linked implementation is `wally_bip39.cpp` (libwally); `bip39.cpp` is an unlinked
reference fallback. Generation pulls **128 bits** from OpenSSL `RAND_bytes` → 12-word
English mnemonic ([`wally_bip39.cpp:50-77`](../src/crypto/wally_bip39.cpp#L50)). The real wallet
path is **full BIP32**: seed → master key → derive `m/44'/19779'/0'/0/0` (coin type
`19779` = `MC_CHAIN_ID`) → feed the child private key to `keypair_from_priv_bytes` with no
rehash ([`wally_bip39.cpp:102-149`](../src/crypto/wally_bip39.cpp#L102)). Same mnemonic + path ⇒
same address as MetaMask. The founder bootstrap uses a different path —
`derive_seed_pbkdf2_sha512` over a memorized passphrase
([`keys.cpp:97-110`](../src/crypto/keys.cpp#L97)).

---

## 3. The block data model

### 3.1 Constants

```cpp
BLOCK_VERSION    = 3;          // block.h:19  — fingerprint-anchored, audio off-chain (Model 1; see §3.2)
SEPARATOR_BYTE   = 0xFF;       // block.h:20
SEPARATOR_LENGTH = 8;          // block.h:21
MAX_BLOCK_SIZE   = 2 MiB;      // block.h:25
MC_CHAIN_ID      = 19779;      // block.h:33  — 0x4D43 "MC", EIP-155 anti-replay
```

A v2 block carries the *fingerprint and content hash* of at most one song, never the
audio bytes. Audio lives in a content-addressed file store and the swarm.

### 3.2 BlockHeader

```cpp
struct BlockHeader {                      // format v3 (block.h)
    uint32_t                   version;     // = 3
    Hash256                    prev_hash;
    Hash256                    merkle_root;
    Hash256                    fingerprint_hash;   // zero on heartbeat
    Hash256                    content_hash;       // zero on heartbeat
    uint64_t                   timestamp_ms;
    // (Model 1) NO confirmations vector — removed in v3.
};
```

> **Updated for Model 1 / format v3.** The `confirmations` vote vector was removed from the
> header (see the status banner). The text below originally described the pre-v3 dual-hash
> scheme; it is kept only to explain *why* the single hash is now correct.

All `Hash256` fields are value-initialized to zero so a default header is all-zero —
which is exactly what a *heartbeat* block (no song) expects.

**Two hashes now** ([`block.cpp`](../src/core/block.cpp)):

- `hash()` = `SHA256(serialize())`. The single canonical block id — the LevelDB key, the
  inv/getdata reference, and (since there are no confirmations to drift) also the anchor
  any tx-signature commitment uses. The old `signing_hash()` was **deleted**.
- `Block::full_hash(serialized)` = `SHA256` over the entire serialized block (used for the
  peer checksum table `k:`).

> ✅ **The dual-hash inconsistency is gone.** Previously validators signed `block.hash()`
> (with confirmations) while receivers/replay verified `signing_hash()` (without) — a latent
> bug (old §21 #2). With confirmations removed from the header there is exactly one hash, so
> the two can no longer disagree. Note: `block.hash()` was always header-only and the song
> *tags* live in the block body, so the editable-tag-vs-immutable-core split (§22.4) already
> holds — tags are outside the hash.

### 3.3 SongSection

Carried only when `has_song == true` ([`block.h:109-125`](../src/core/block.h#L109)):
`audio_format` (1 byte tag, `AudioFormat` enum), `content_hash`, base64
`compressed_fingerprint`, `duration_ms`, `title`, `artist`, `artist_address`, `genre`,
`album`, optional `year`/`track_number`, and a vector of `RoyaltySplit{address,
basis_points}` (basis points must sum to 10000).

### 3.4 Confirmation

```cpp
struct Confirmation {           // block.h:129-133
    Hash256  validator_id;      // 32 bytes
    PubKey33 pubkey;            // 33 bytes
    Sig64    signature;         // 64 bytes
};                              // wire size: 129 bytes
```

### 3.5 Serialization byte layout

`BlockHeader::serialize` ([`block.cpp:68-84`](../src/core/block.cpp#L68)), little-endian throughout:

```
u32  version
32B  prev_hash
32B  merkle_root
32B  fingerprint_hash
32B  content_hash
u64  timestamp_ms
u16  confirmations.size()
  per confirmation: 32B validator_id | 33B pubkey | 64B signature
```

`Block::serialize` ([`block.cpp:104-144`](../src/core/block.cpp#L104)) appends:

```
header bytes (above)
u8   has_song (0x00 / 0x01)
if has_song:
    u8    audio_format
    32B   content_hash
    str16 compressed_fingerprint     (u16 len + bytes)
    u32   duration_ms
    str16 title | str16 artist | 20B artist_address | str16 genre | str16 album
    u16   year | u16 track_number
    u8    royalty_splits.size()
      per split: 20B address | u16 basis_points
8B   separator (0xFF × 8)
u32  transactions.size()
  per tx: u32 len | <len> raw tx bytes
```

`Block::deserialize` ([`block.cpp:146-211`](../src/core/block.cpp#L146)) is the exact inverse and is
bounds-checked at every read (`read_*` helpers return false on short buffers,
[`block.cpp:32-64`](../src/core/block.cpp#L32)); the 8-byte separator is verified byte-for-byte
([`block.cpp:192-196`](../src/core/block.cpp#L192)). Any malformed input ⇒ `false` ⇒ the block is
dropped upstream.

### 3.6 `Block::validate()` — intrinsic self-consistency

This is the first gate every block passes, locally produced or received
([`block.cpp:243-266`](../src/core/block.cpp#L243)):

1. **Heartbeat blocks** (`!has_song`): both `header.fingerprint_hash` and
   `header.content_hash` MUST be zero, else reject.
2. **Song blocks**: `header.content_hash` MUST equal `song.content_hash`, AND
   `header.fingerprint_hash` MUST equal `sha256(song.compressed_fingerprint)`.
   (Audio bytes are *not* checked here — that is the Deep Auditor's job, §16.)
3. **Merkle**: `compute_merkle_root(transactions)` MUST equal `header.merkle_root`.

Note what `validate()` does **not** check: prev_hash linkage, confirmation quorum,
signatures, duplicate songs, or transaction validity. Those are layered on by
`Chain::validate_block` (§6) and the consensus/replay paths (§9, §15).

---

## 4. Transactions

Transactions are raw byte blobs inside `block.transactions`. The first byte is the
`TxType` tag ([`transaction.h:9-17`](../src/core/transaction.h#L9)):

| Tag | Type | Purpose |
|---|---|---|
| `0x01` | `TRANSFER` | Move MC between accounts ✅ (inline `from_pubkey`; see §4.2/§7.1) |
| `0x10` | `MINT` | Play-reward minting (carries a signed `PlayProof`) |
| `0x20` | `MODERATOR_OP` | GRANT / REVOKE / TAG_LABEL_EDIT |
| `0x30` | `MODERATOR_PROPOSAL` | HIDE_CONTENT / RELEASE_ESCROW / VOTE_YES |
| `0x40` | `USERNAME_REGISTER` | First-come username claim |
| `0x50` | `SLASH` | Equivocation / fingerprint-forgery evidence |
| `0x60` | `RELAY_REWARD` | Credit a mini-node for relayed traffic |

### 4.1 The signing convention (EIP-155 style)

Every signed tx has a `sign_message()` that **prepends `MC_CHAIN_ID` (u32 LE)** to the
preimage, then SHA-256s it, then ECDSA-signs that hash. The chain_id is mixed into the
*signature* but **not transmitted** in the serialized bytes — verifiers rebuild the
preimage with their own `MC_CHAIN_ID` ([`transaction.cpp:178-182`](../src/core/transaction.cpp#L178)).
This means a MusicChain signature can never replay on Ethereum/BSC/Base (their chain_id
won't match) and vice-versa.

The canonical `verify_signature()` for *most* types (Mint excluded — see below) is:

```cpp
auto hash = sha256(sign_message());
Address derived = address_from_pubkey(<inline pubkey>);
if (memcmp(derived, <claimed address>, 20) != 0) return false;  // pubkey binds to addr
return verify_ecdsa(hash, signature, <inline pubkey>);
```

i.e. the signer's pubkey travels *inside* the tx, is cross-checked against the address the
chain will act on, and then verifies the signature. This sidesteps the missing key-recovery
(§2.4) — except for `TransferTx`, which does not carry a pubkey.

### 4.2 TransferTx ✅ FIXED

```cpp
struct TransferTx {             // transaction.h
    Address from_address, to_address;
    uint64_t amount, nonce;
    PubKey33 from_pubkey;       // ← added: inline compressed pubkey
    Sig64    signature;
};
```

Preimage: `chain_id | from | to | amount | nonce | from_pubkey`
([`transaction.cpp`](../src/core/transaction.cpp)). `verify_signature()` now cross-checks
`address_from_pubkey(from_pubkey) == from_address` then calls `verify_ecdsa(hash, signature,
from_pubkey)` — the same inline-pubkey pattern every other signed tx uses. **Transfers now
validate and mine.** The old always-false `verify_ecdsa_from_address` path is dead (only the
unreachable legacy HTTP moderator route still references it). `post_transfer`
([`server.cpp`](../src/api/server.cpp)) requires a `from_pubkey` field.

### 4.3 PlayProof + MintTx

`PlayProof` ([`transaction.h:80-98`](../src/core/transaction.h#L80)) is the node-signed evidence of a
play: `session_id`, `content_hash`, `block_hash`, `artist_address`, `player_address`,
`serving_node_id`, start/end timestamps, `total_duration_ms`, `heartbeat_count`, and a
`node_signature`. The signing node is the full node that ran the session
(`HttpServer::post_session_complete` signs it with `node_keypair_`, §17.4).

`MintTx` ([`transaction.h:109-117`](../src/core/transaction.h#L109)) wraps a `PlayProof`, a vector of
computed `MintOutput{recipient, amount}`, and a `burn_amount`. The mint outputs are
*computed at processing time* by `compute_mint_outputs` (§8.4), not signed by a user —
the trust anchor is the node signature on the embedded `PlayProof`. `burn_amount` is an
optional trailing field for backward compat ([`transaction.cpp:145-147`](../src/core/transaction.cpp#L145)).

### 4.4 Other signed types (all use inline-pubkey verify)

- **ModeratorOpTx** ([`transaction.h:134-151`](../src/core/transaction.h#L134)): `op_code`, `level`,
  `subject` + `subject_pubkey`, `proposer` + `proposer_pubkey`, `nonce`, a ≤4 KiB
  `meta_json` (for `TAG_LABEL_EDIT`), signature. `meta_len > 4096` is rejected at
  deserialize ([`transaction.cpp:212`](../src/core/transaction.cpp#L212)).
- **ProposalTx** ([`transaction.h:184-203`](../src/core/transaction.h#L184)): `kind`, the union of
  `target_hash` / `target_addr` / `amount`, `proposer` + pubkey, `nonce`, signature.
  Unused union fields must be zero or the chain rejects the tx (§7.4).
- **UsernameTx** ([`transaction.h:217-230`](../src/core/transaction.h#L217)): `name` (3–30 chars,
  length-validated at deserialize, [`transaction.cpp:337`](../src/core/transaction.cpp#L337)), `owner`
  + pubkey, `nonce`, signature.
- **RelayRewardTx** ([`transaction.h:257-271`](../src/core/transaction.h#L257)): `target_address`
  (mini-node wallet), `count`, `issuer_address` + pubkey (founder), `nonce`, signature.
- **SlashTx** ([`transaction.h:294-310`](../src/core/transaction.h#L294)): `kind`, `target_address` +
  `target_pubkey`, length-prefixed `evidence` (capped at `MAX_BLOCK_SIZE`,
  [`transaction.cpp:456`](../src/core/transaction.cpp#L456)), `nonce`, `reporter_address` + pubkey,
  signature.

Every one of these computes `sha256(sign_message())`, cross-checks
`address_from_pubkey(inline_pubkey) == claimed_address`, then `verify_ecdsa`. Tampering
with any signed field, or signing with a key whose address doesn't match, fails
verification.

---

## 5. Merkle root

`Block::compute_merkle_root` ([`block.cpp:213-237`](../src/core/block.cpp#L213)):

1. Empty tx list ⇒ all-zero `Hash256` (this is why heartbeat blocks have a zero merkle
   root, consistent with `Block::validate`).
2. Leaf = `sha256(tx_bytes)` per transaction.
3. Bottom-up pairing: if a level has an odd count, **duplicate the last hash**
   (Bitcoin-style), then `parent = sha256(left || right)` until one root remains.

`merkle.h`/`merkle.cpp` are thin standalone wrappers that delegate to this
([`merkle.h:8-11`](../src/core/merkle.h#L8)). There is no merkle *proof* / SPV machinery — the
root only exists so `Block::validate` can detect tampered tx sets.

---

## 6. The chain: block acceptance & state transition

`mc::Chain` ([`chain.h`](../src/core/chain.h), [`chain.cpp`](../src/core/chain.cpp)) owns the canonical
chain and all derived state. A single `std::mutex mu_` serializes the producer thread,
the network thread, and RPC threads so the tip can't tear ([`chain.h:178-184`](../src/core/chain.h#L178)).

### 6.1 `connect_block` — the one authoritative entry point

Every block — genesis, locally minted, or received — becomes canonical only through
`connect_block` ([`chain.cpp:45-119`](../src/core/chain.cpp#L45)). Under the chain lock:

1. **`validate_block(block, err)`** (§6.2). On failure, log and return false — nothing is
   written.
2. Build a single `leveldb::WriteBatch` (atomic; either the whole block lands or none of
   it).
3. **Persist block**: `b:<hash>` = serialized block; `h:<hash>` = height (u32);
   `n:<height>` = hash; `k:<height>` = full-block checksum; `t:tip` = `hash || height`
   ([`chain.cpp:60-78`](../src/core/chain.cpp#L60)).
4. **Index song state** (only if `has_song`): fingerprint index, song metadata, artist
   index, genre index, content→height map ([`chain.cpp:84-90`](../src/core/chain.cpp#L84)). Heartbeat
   blocks skip this so the indexes aren't corrupted with zero hashes.
5. **`apply_transactions(block, new_height, batch)`** (§6.3 / §7). On failure ⇒ return
   false; the batch is discarded, so a single bad tx aborts the whole block.
6. **Drain mempool in the same batch**: for each tx, `del p:<sha256(tx)>`
   ([`chain.cpp:107-111`](../src/core/chain.cpp#L107)). This closes a crash window where a tx was applied
   but left pending and would be re-applied (and trip the nonce check) on next startup
   ("Bug fix #6").
7. **`db_.write(batch)`** — the atomic commit. Only after it succeeds is the in-memory
   `tip_` updated ([`chain.cpp:113-118`](../src/core/chain.cpp#L113)).

### 6.2 `validate_block` — the acceptance gate

([`chain.cpp:831-848`](../src/core/chain.cpp#L831)), runs under the chain lock:

1. **`block.validate()`** — intrinsic consistency (§3.6). Fail ⇒ `"block internal
   validation failed"`.
2. **`block.header.prev_hash == tip_.hash`** — strict linkage to the current tip. Fail ⇒
   `"prev_hash mismatch"`. This is what enforces a single linear chain at connect time.
3. **Duplicate-song guard** (song blocks only): if `db_.get_fingerprint(content_hash)`
   already exists ⇒ `"duplicate song"`.

Notably, `validate_block` does **not** itself verify confirmation quorum or signatures —
that gate lives in the consensus/replay/propagation layers (§9, §14, §15). `connect_block`
trusts that whoever called it already established quorum. This is a deliberate split: the
*producer* establishes quorum before calling connect; the *propagator* verifies ≥1 valid
sig before calling connect (§14.2); *replay* re-verifies on startup (§15).

There are two relaxed siblings:

- **`validate_candidate`** ([`chain.cpp:854-871`](../src/core/chain.cpp#L854)): same as `validate_block`
  but **skips the prev_hash check**, because a follower validating a broadcast candidate
  may not yet have applied the producer's previous block. The strict check still happens
  later at `connect_block`, so a structurally-fine-but-unlinkable candidate simply won't
  connect.
- **`validate_block_quick_duplicate`** ([`chain.cpp:850-852`](../src/core/chain.cpp#L850)): a fast
  content-hash existence probe used by the producer's re-queue logic.

### 6.3 `apply_transactions` — deterministic state transition

([`chain.cpp:150-227`](../src/core/chain.cpp#L150)). For the block being connected at `height`:

1. Reset per-block staging: `proposal_votes_in_block_.clear()` and
   `applied_nonce_in_block_.clear()` ([`chain.cpp:154-155`](../src/core/chain.cpp#L154)). These let
   multiple txs from one address (or a proposal + its first vote) in the *same* block see
   each other's effects before the batch flushes.
2. For each non-empty raw tx, switch on `raw_tx[0]` (the TxType byte) and dispatch to the
   matching `apply_*` (§7). **Any deserialize failure or any apply failure returns false
   for the whole block.** There is no "skip the bad tx" — a block is all-or-nothing.
3. **Unknown TxType ⇒ reject the block** ([`chain.cpp:221-224`](../src/core/chain.cpp#L221)). This means
   a node that doesn't understand a future tx type will refuse blocks containing it (hard
   fork semantics).

### 6.4 The in-block nonce machine

Two helpers make multiple same-sender txs work within one block
([`chain.cpp:132-148`](../src/core/chain.cpp#L132)):

- `next_expected_nonce(addr)` returns `applied_nonce_in_block_[addr]` if this address
  already transacted earlier in the block, else `db_.get_nonce(addr)`.
- `record_applied_nonce(addr, new_value)` stores the new floor.

Every signed apply uses **strict equality** (`tx.nonce != expected ⇒ reject`), so nonces
must be gapless and monotonic per address. The canonical reason this exists: bootstrap
emits a founder GRANT (nonce 0) immediately followed by a UsernameTx (nonce 1) for the
same address in the same block.

---

## 7. Per-transaction-type apply rules

All `apply_*` functions take the shared `WriteBatch` and return `bool`. A `false` from any
of them aborts the entire block (§6.3).

### 7.1 `apply_transfer` ✅ FIXED

([`chain.cpp:121-130`](../src/core/chain.cpp#L121)): verify signature → check nonce (strict) →
`Ledger::transfer` → advance nonce. With the TransferTx inline-pubkey fix (§4.2),
`verify_signature()` now succeeds for a correctly signed transfer, so this function applies
and TRANSFER blocks connect.

### 7.2 `apply_mint`

([`chain.cpp:229-289`](../src/core/chain.cpp#L229)), the play-reward path:

1. **Zero-recipient guard**: any output to the zero address ⇒ reject (defense in depth,
   [`chain.cpp:238-241`](../src/core/chain.cpp#L238)).
2. **Hard supply cap**: sum the outputs; if `current_supply + mint_total > SUPPLY_CAP` ⇒
   reject ([`chain.cpp:247-254`](../src/core/chain.cpp#L247)).
3. **Burn** (post-10k-play deflation): if `burn_amount > 0`, re-check the player's
   balance, `Ledger::debit` it, and **decrement `total_supply`**
   ([`chain.cpp:257-264`](../src/core/chain.cpp#L257)).
4. **Replay marker** written *before* crediting: `u:<session_id>` = `{}`
   ([`chain.cpp:271`](../src/core/chain.cpp#L271)). A reorg/replay on an already-minted session can't
   double-credit.
5. **Song state**: `update_song_state` bumps `play_count` and records the discoverer on
   first play.
6. **Credit** all outputs via `Ledger::credit_many` (aggregates per-address, bumps supply
   once — §8.2).

Note `apply_mint` does **not** re-verify the embedded `PlayProof.node_signature`; the
proof is trusted because the block producer is the node that signed it, and the
session/replay machinery in the API layer (§17.4) gates it before it ever reaches a block.

### 7.3 `apply_moderator_op`

([`chain.cpp:291-420`](../src/core/chain.cpp#L291)): verify signature → strict nonce → switch on op:

- **GRANT** ([`chain.cpp:313-346`](../src/core/chain.cpp#L313)):
  - **Bootstrap self-grant**: if there is no founder yet AND `level == FOUNDER` AND
    `proposer == subject`, set the subject to FOUNDER, record its pubkey + active height,
    and persist `founder`. This is the **one and only permitted self-grant on the entire
    chain** — it is how the network is born.
  - Otherwise: founder must exist, proposer must be FOUNDER, the new level cannot be NONE
    or FOUNDER (exactly one founder ever), and you can't re-grant the founder.
- **REVOKE** ([`chain.cpp:347-360`](../src/core/chain.cpp#L347)): founder-only; can't revoke the
  founder; target must currently hold a level.
- **TAG_LABEL_EDIT** ([`chain.cpp:361-417`](../src/core/chain.cpp#L361)): founder-only JSON-described
  metadata. `label_define` validates that wallet splits parse and **sum to exactly 10000
  bp** with 1–16 splits; `label_assign` binds an artist to a label (empty label clears).
  Any malformed payload ⇒ reject.

Every successful branch advances the proposer nonce.

### 7.4 `apply_proposal` (multi-moderator governance)

([`chain.cpp:714-809`](../src/core/chain.cpp#L714)): verify signature → strict nonce → require
`proposer_level >= OP`. Quorum is a **strict majority of currently-active moderators**:
`needed = floor(active_n / 2) + 1` ([`chain.cpp:732-733`](../src/core/chain.cpp#L732)). A single-mod
chain (just the founder) executes immediately on the proposer's implicit YES.

- **HIDE_CONTENT**: requires zeroed `target_addr`/`amount` and a non-zero `target_hash`
  (the content hash); store proposal, record proposer's vote, execute if quorum reached.
- **RELEASE_ESCROW**: requires zeroed `target_hash`, non-zero `target_addr`, non-zero
  `amount`; same store/vote/execute. Execution (`execute_proposal`,
  [`chain.cpp:642-712`](../src/core/chain.cpp#L642)) computes the deterministic escrow address, caps
  the release at the current escrow balance, and — if the artist is assigned to a record
  label — splits the payout across the label's wallets by basis points with dust going to
  the last split.
- **VOTE_YES**: requires the referenced proposal to exist and be PENDING, rejects
  double-votes (both on-disk and in-block), records the vote, and executes the *stored*
  proposal if quorum is now met.

The "unused union fields must be zero" rule ([`transaction.h:172-176`](../src/core/transaction.h#L172))
keeps the tx hash canonical so the same logical proposal always hashes identically.

### 7.5 `apply_username_register`

([`chain.cpp:438-466`](../src/core/chain.cpp#L438)): verify signature → strict nonce →
well-formedness (`[a-z0-9_]`, 3–30 chars, must start with a letter,
[`chain.cpp:425-435`](../src/core/chain.cpp#L425)) → name not already taken → owner doesn't already
have a username → persist `un:` + `addrun:` mappings → advance nonce. First-come,
first-served; usernames grant no privilege, only a public reverse lookup.

### 7.6 `apply_relay_reward`

([`chain.cpp:470-511`](../src/core/chain.cpp#L470)): verify issuer signature → **issuer must be the
founder** (Phase 2; widens to any validator later) → strict nonce → `count` in
`(0, 1_000_000]` → credit `count × 100_000_000` internal units (1 MC per relayed unit) to
`target_address` → advance nonce. Note: this calls `Ledger::credit`, which **does** bump
total supply but **has no supply-cap guard** — see §8 and §21 for the asymmetry.

### 7.7 `apply_slash`

([`chain.cpp:515-609`](../src/core/chain.cpp#L515)): see §11.

---

## 8. Token economics & the ledger

### 8.1 Units & supply bounds

```cpp
TOKEN_DECIMALS = 100000000;                          // ledger.h:10  — 8 decimals
SUPPLY_FLOOR   = 1e9 tokens × TOKEN_DECIMALS;        // ledger.h:33  — 1 billion
SUPPLY_CAP     = 2e9 tokens × TOKEN_DECIMALS;        // ledger.h:34  — 2 billion
```

Balances are a flat `a:<addr>` → u64 map; total supply is the single key
`c:total_supply` ([`database.cpp:151-166`](../src/storage/database.cpp#L151)).

### 8.2 Ledger operations

([`ledger.cpp`](../src/tokens/ledger.cpp)):

- `credit(batch, addr, amount)` reads disk balance, adds, writes, **and bumps
  total_supply** ([`ledger.cpp:42-49`](../src/tokens/ledger.cpp#L42)). ⚠ Not safe to call twice for
  the same address in one batch (it re-reads the *disk* balance, missing the pending
  write).
- `credit_many(batch, outs)` aggregates per-address into a map first, then writes each and
  bumps supply once with the total ([`ledger.cpp:51-70`](../src/tokens/ledger.cpp#L51)). This is the
  safe multi-output primitive `apply_mint` uses.
- `debit` returns false on insufficient balance; **does not touch supply** (the caller
  adjusts supply for burns).
- `transfer` is a supply-neutral debit+credit; self-transfer is a no-op.

The split — credit bumps supply, debit/transfer don't, burn is a manual supply decrement
in `apply_mint` — is the source of the relay-reward supply-cap asymmetry noted in §21.

### 8.3 The burn curve

`compute_burn_rate(total_supply)` ([`ledger.cpp:10-21`](../src/tokens/ledger.cpp#L10)):

- `supply < SUPPLY_FLOOR` (under 1B) ⇒ **0 burn**. "Give the network away" bootstrap mode.
- `supply >= SUPPLY_CAP` (≥ 2B) ⇒ `UINT64_MAX` ⇒ `apply_mint` refuses ⇒ chain frozen.
- In between ⇒ cubic ramp: `pct = (supply − floor) / (cap − floor)`,
  `burn = pct³ × 1000 × TOKEN_DECIMALS`. Documented points: 1.5B ≈ 125 tokens, 1.8B ≈ 512,
  1.9B ≈ 729 ([`ledger.h:43-49`](../src/tokens/ledger.h#L43)). Cubic so deflation accelerates near
  the cap.

### 8.4 Mint reward routing — `compute_mint_outputs`

([`mint.cpp:7-113`](../src/tokens/mint.cpp#L7)). Reward constants: artist / serving-node / discoverer
each = **1 token** (`100000000` units, [`ledger.h:16-23`](../src/tokens/ledger.h#L16)). The *tier*
boundary is `FULL_REWARD_THRESHOLD = 10000` plays.

- **Royalty-split validation**: splits valid only if non-empty AND
  `sum(basis_points) <= 10000`; the leftover bp routes to a fallback escrow
  ([`mint.cpp:24-28`](../src/tokens/mint.cpp#L24)).
- **Pre-10k tier** (`play_count < 10000`, [`mint.cpp:30-73`](../src/tokens/mint.cpp#L30)): the artist
  share (1 token) goes to **escrow** (per-split escrow addresses, or the artist's escrow,
  or the zero escrow for artist-less songs); the serving node and the discoverer each get
  1 spendable token; **no burn**.
- **Post-10k tier** (`play_count >= 10000`, [`mint.cpp:74-110`](../src/tokens/mint.cpp#L74)): the
  artist is paid **directly** (no escrow); the serving node gets 1 token; the **listener
  earns nothing and instead burns** `compute_burn_rate(total_supply)`.

So a song's first 10 000 plays park the artist's earnings in escrow (releasable by
moderator vote, §7.4) and pay listeners to discover; after 10 000 plays the artist is paid
directly and listening costs a (supply-dependent) burn.

---

## 9. Consensus: candidates, quorum, confirmations

> **⚠ REMOVED FROM CODE (historical).** This entire "producer-proposes / validators-co-sign"
> scheme — `BlockCandidate`, `dynamic_quorum`, `MAX_CONFIRMATIONS`, `add_confirmation`, the
> `mc:block_candidate`/`mc:block_confirmation` channels, the confirmation wait in
> `commit_block`, and the `node_main` candidate/confirmation handlers — was **deleted** in
> the Model 1 pass. The "check uniqueness, then vote" flow in §9.4 no longer exists. Block
> production is now build → `connect_block` → announce; validity is re-derived
> deterministically by every node (§22). This section is retained only as a record of what
> was removed; the file:line refs below point at code that is gone.

Consensus is a lightweight **producer-proposes / validators-co-sign** scheme, *not*
proof-of-work or staked proof-of-stake. Source: `CandidateManager`
([`candidate.h`](../src/consensus/candidate.h), [`candidate.cpp`](../src/consensus/candidate.cpp)) plus the
handlers wired in `node_main.cpp`.

### 9.1 Constants

```cpp
MAX_CONFIRMATIONS     = 5;            // candidate.h:26  — ceiling the network ever demands
BLOCK_TIMEOUT_SECONDS = 300;         // candidate.h:43  — candidate expiry & confirm wait
HEARTBEAT_INTERVAL_MS = 5 * 60 * 1000; // candidate.h:49 — empty-block cadence
```

### 9.2 The quorum rule ⚠ REMOVED — Model 1 has no quorum

> **Deleted in the Model 1 pass.** Under vote-free deterministic consensus there is **no
> quorum and no confirmation counting at all.** A block is accepted by each node's *own*
> deterministic validity check — parent linkage plus per-tx re-validation (one-song-once
> uniqueness, fingerprint/deep-audit, signatures, nonces, config checkpoints) — and the
> network converges by **fork choice on the heaviest valid chain** (cumulative audited
> plays, §20.1/§22.5), never by a signature tally. `dynamic_quorum()` was deleted with the
> rest of the producer/co-sign machinery — `BlockCandidate`, `MAX_CONFIRMATIONS`,
> `add_confirmation`, the candidate/confirmation channels — see the §9 banner above, §21 #3,
> and §22. The header comment now states this outright
> ([`candidate.h:23-28`](../src/consensus/candidate.h#L23)). The snippet below is the *deleted*
> code, kept only as a record of what "quorum" used to mean.

```cpp
// DELETED — no longer in candidate.h. Historical only.
inline uint32_t dynamic_quorum(size_t peer_count) {
    if (peer_count == 0) return 1;
    return 2;
}
```

Historically this hard-capped at **2** signatures regardless of validator count: 0 peers ⇒
the producer self-signed; any peers ⇒ producer + one co-signer. `peer_count` was the raw
librats validated-peer set — which includes non-validator DHT peers that would never
co-sign — so a "scale quorum with peers" formula would have hung the producer forever
waiting for confirmations nobody could give. Model 1 dissolves the problem entirely:
`commit_block` is now build → `connect_block` → announce with **no confirmation wait**
([`candidate.h:120-133`](../src/consensus/candidate.h#L120)), so there is no quorum to size.

### 9.3 commit_block — the two paths

`commit_block` ([`candidate.cpp:192-429`](../src/consensus/candidate.cpp#L192)) is the gate every minted
block passes:

- **Genesis fast path** (`tip().height == 0`, [`candidate.cpp:224-285`](../src/consensus/candidate.cpp#L224)):
  self-sign `quorum = 1` confirmation(s), each with a distinct
  `validator_id = sha256("solo:" || node_id || u32_be(i))` (distinct ids because
  `add_confirmation` dedups by id), set `header.confirmations`, call `connect_block`
  directly, announce, persist a bucketed `.blk` dump. **There is no path back to this
  fast self-sign after genesis.**
- **Multi-node path** (height ≥ 1, [`candidate.cpp:287-428`](../src/consensus/candidate.cpp#L287)):
  - `quorum = dynamic_quorum(network.peer_count())`.
  - Register the candidate under its hex hash.
  - **If `peer_count() == 0`** ([`candidate.cpp:321-343`](../src/consensus/candidate.cpp#L321)): self-sign
    `quorum` (= 1) confirmations with distinct solo ids; mark confirmed; connect.
  - **If peers exist** ([`candidate.cpp:344-376`](../src/consensus/candidate.cpp#L344)): self-sign one
    slot (counting the producer's own vote), `network.publish_candidate(serialize())` to
    fan it out, then **block on a condition variable** until `is_final()` or the 300s
    deadline. On timeout ⇒ `"Confirmation timeout"` and the block is abandoned. On quorum
    ⇒ copy the accumulated confirmations onto the block and `connect_block`.

### 9.4 What a validator does on receiving a candidate

`node_main.cpp`'s `set_block_candidate_handler` ([`node_main.cpp:463-558`](../tools/node_main.cpp#L463)):

1. `Block::deserialize`; drop if malformed.
2. `chain.validate_candidate(block, err)` — structural validation, prev_hash skipped (§6.2).
3. **Song uniqueness gate #1**: reject if `db.get_fingerprint(content_hash)` exists.
4. **Song uniqueness gate #2**: fuzzy chromaprint probe; reject if any existing song is
   `>= 0.55` similar.
5. On pass, broadcast a `Confirmation`:
   `validator_id = cfg.node_id`, `pubkey` = node pubkey,
   **`signature = sign_ecdsa(block.hash(), private_key)`** ([`node_main.cpp:547-555`](../tools/node_main.cpp#L547)).

> ✅ **The signing-hash discrepancy is gone (format v3).** This used to be a latent
> correctness bug: validators signed `block.hash()` (the full header, *including* any
> confirmations already on the candidate) while the receive and replay paths re-verified
> against a `block.signing_hash()` that *cleared* confirmations — two different preimages
> over the same block. In Model 1 the `confirmations` vector was removed from `BlockHeader`
> entirely and `signing_hash()` was deleted with it ([`block.h`](../src/core/block.h),
> [`block.cpp`](../src/core/block.cpp)). There is now exactly one hash function,
> `header.hash()`, over a header that has no vote field, so producers/validators and
> receivers necessarily hash the same bytes — the dual-preimage drift is structurally
> impossible. See §21 #2 and §22. (A `Confirmation` struct still exists, but only as
> `SlashTx` evidence; it is never written into a header.)

### 9.5 Accumulating confirmations

The producer's `set_confirmation_handler` ([`node_main.cpp:561-585`](../tools/node_main.cpp#L561)):

1. Derive the signer address from `c.pubkey`; **drop the confirmation if
   `chain.is_slashed(addr)`** — this is the live slashing enforcement point (§11).
2. `candidates.add_confirmation(block_hash_hex, c)`.

`add_confirmation` ([`candidate.cpp:55-71`](../src/consensus/candidate.cpp#L55)) dedups by
`validator_id`, appends to `received_confirmations`, mirrors into
`header.confirmations`, and when `received_confirmations.size() >= required_quorum`
notifies the producer's condition variable. **It does not re-verify the signature** — the
only gate at the producer is the slashed-address filter. (Signature authenticity is
enforced by the propagation receiver and by replay, just against the other hash — see the
discrepancy above.)

### 9.6 The validator registry ⚠ REMOVED

`ValidatorRegistry` was **deleted** in the Model 1 pass (see the §9 banner above and §21 #3).
It used to persist records under `v:` and track a reliability EWMA, but it was never
consulted by consensus — `get_active_validators()` returned empty, `active_count()` was
always 0, `MIN_VALIDATORS = 5` was unused — and with quorum/voting gone there was nothing
left for it to feed, so it went with the rest of the producer/co-sign machinery. The `v:`
key prefix it wrote is now dead (§12, §21 #9). Validity is re-derived deterministically by
every node (§22), not by any registered/weighted validator set.

---

## 10. Block production loop & timing

`CandidateManager::start` ([`candidate.cpp:98-133`](../src/consensus/candidate.cpp#L98)) is gated by
`cfg.validator_enabled`: false ⇒ the node is a follower (syncs and serves, never
produces); true ⇒ it spawns `heartbeat_loop`. `start` deliberately leaves
`last_block_at_ms_ = 0` so the first iteration immediately sees the heartbeat window
exceeded.

The producer loop ([`candidate.cpp:433-747`](../src/consensus/candidate.cpp#L433)) wakes on three
signals: a queued song registration, a 30-second poll, or `stop()`. Each iteration:

1. Drain at most one `PendingRegistration` from the queue.
2. Pull the whole pending-tx mempool (`db.get_all_pending_txs()`).
3. **Mempool pre-flight** ([`candidate.cpp:497-627`](../src/consensus/candidate.cpp#L497)): deserialize
   and `verify_signature()` each pending tx by type; drop invalid ones from the mempool;
   **stable-sort survivors by `(sender, nonce)`** so per-sender nonces stay monotonic in
   the block.
4. Idle short-circuit: nothing queued + empty mempool + still inside the 5-minute
   heartbeat window ⇒ `continue` (no block).
5. Pre-genesis guard: at height 0 with no registration and no txs ⇒ `continue` (don't turn
   an empty heartbeat into genesis).
6. Build the block: `version`, `prev_hash = tip().hash`, `timestamp_ms = now`, push tx
   bytes, compute merkle root. If a registration is present, set `has_song`, copy song
   metadata, set `header.content_hash`, and **always recompute**
   `header.fingerprint_hash = sha256(compressed_fingerprint)` so `Block::validate` is
   tautologically satisfied.
7. `commit_block(...)` (§9.3). On failure, apply the retry policy: bump retries, give up at
   ≥3 or if the content hash is now a known duplicate, else re-queue.

So: a block is produced **on demand** when a song is registered or a tx is queued (the
producer is `wake()`-d), and otherwise **every 5 minutes** as an empty heartbeat to keep
the chain advancing and timestamps fresh.

---

## 11. Slashing

Design ([`slashing.h:12-91`](../src/consensus/slashing.h#L12)) names three offenses: **equivocation**
(one validator signs two distinct height-N blocks), **fingerprint forgery** (audio doesn't
match the declared fingerprint, from the Deep Auditor, §16), and **double-vote**. The
intended effect is to zero the offender's confirmation weight permanently.

### 11.1 Proofs

- `EquivocationProof::verify()` ([`slashing.cpp:6-18`](../src/consensus/slashing.cpp#L6)): both
  confirmations share a `validator_id`, name distinct block hashes, and each signature
  verifies via `verify_ecdsa(block_x_hash, conf_x.signature, conf_x.pubkey)`.
- `FingerprintForgeryProof::verify()` ([`slashing.cpp:20-33`](../src/consensus/slashing.cpp#L20)): the
  reporter's signature over the block hash verifies, and `similarity < 0.50`.

### 11.2 What is actually wired

Contrary to the "reserved for future code" framing, the **application path is live**:

`Chain::apply_slash` ([`chain.cpp:515-609`](../src/core/chain.cpp#L515)):

1. Verify reporter signature; strict reporter nonce.
2. `address_from_pubkey(target_pubkey) == target_address`.
3. **EQUIVOCATION**: parse the fixed `kEvidenceLen = 4 + 129 + 129 + 32 + 32` evidence,
   bind both confirmations' pubkeys to `target_pubkey`, run `EquivocationProof::verify()`.
4. **FINGERPRINT_FORGERY** ⚠: currently **trusts the reporter signature and accepts
   (`ok = true`)** without re-running chromaprint or multi-validator confirmation
   ([`chain.cpp:581-592`](../src/core/chain.cpp#L581)).
5. On success, write `slashed:<target>` = `{1}` and advance the reporter nonce. Idempotent.

`Chain::is_slashed(addr)` ([`chain.cpp:611-613`](../src/core/chain.cpp#L611)) is consulted live at the
producer's confirmation handler (§9.5) to drop slashed validators' votes.

### 11.3 The gaps — largely moot under Model 1

Slashing is present-but-**vestigial** skeleton. The apply/verify plumbing exists and is
reachable, but nothing ever *produces* a `SlashTx`, and its main consumer (confirmation
weighting) no longer exists.

- **The Deep Auditor no longer only logs.** On a detected forgery it (1) calls
  `db_.mark_song_deleted` locally and (2) fires an `on_forgery` callback that `node_main`
  wires to `RatsApi::publish_forgery_report` — a **node-signed** off-chain `forgery_report`
  gossiped across the mesh and corroborated by `kForgeryQuorum = 2` distinct reporters (or a
  single independent local re-audit) ([`deep_audit.cpp`](../src/sync/deep_audit.cpp),
  [`rats_api.cpp`](../src/api/rats_api.cpp) `publish_forgery_report`/`handle_forgery_report`).
  This replaced the on-chain slash path in practice — see §16 and §21 #4.
- **`EQUIVOCATION` is dead by architecture.** It needs two block-level `Confirmation`s from
  one validator at one height, but Model 1 stores **no block-level votes at all**
  ([`block.h`](../src/core/block.h), [`candidate.h:23-28`](../src/consensus/candidate.h#L23)),
  so the evidence can never arise on-chain.
- **Confirmation-weight is moot.** It zeroed a weight in a validator registry; that registry
  and the `MAX_CONFIRMATIONS`/`dynamic_quorum`/`BlockCandidate` machinery were all removed
  (§9, §21 #3). Only the boolean `slashed:` marker remains, with no live confirmation-tally
  reader.
- **No `SlashTx` is emitted anywhere.** There is no automatic detector, and nothing
  constructs/signs/submits one — the only references are the apply-path deserialize and the
  verify-time `EquivocationProof` inside `apply_slash`. `FINGERPRINT_FORGERY` apply remains an
  accept-only stub (trusts the reporter sig, no semantic re-check).

So the `slashed:` marker plus the `SlashTx` apply path are dormant plumbing with no emitter
and no live reader; `EQUIVOCATION` and confirmation-weight are dead-by-architecture under
Model 1; and forgery enforcement now lives in the off-chain `forgery_report` flow (§16).

---

## 12. Storage: the LevelDB schema

One LevelDB at `<data_dir>/blockchain.db`, opened with a 64 MiB write buffer, 256 MiB LRU
block cache, a 10-bit bloom filter, and Snappy ([`database.cpp:38-48`](../src/storage/database.cpp#L38)).
All integers are little-endian. Batched writes use `sync=false` (atomic, no fsync).

Key prefixes (the complete map):

| Prefix | Key body | Value | Purpose |
|---|---|---|---|
| `b:` | `hex(block_hash)` | serialized `Block` | the block store |
| `h:` | `hex(block_hash)` | u32 | block → height |
| `n:` | `height` | 32B hash | height → block hash |
| `k:` | `height` | 32B hash | full-block checksum (peer verify) |
| `t:tip` | — | `hash(32) ‖ height(4)` | chain tip pointer |
| `a:` | `hex(addr)` | u64 | account balance |
| `c:total_supply` | — | u64 | total minted supply |
| `nv:` | `hex(addr)` | u64 | per-account nonce |
| `u:` | `hex(session_id)` | — | mint-session replay marker |
| `s:` | `hex(content_hash)` | `SongState` (40B) | play_count, discoverer, first-play |
| `f:` | `hex(content_hash)` | fp len + fp + block_hash | fingerprint index |
| `fph:` | `hex(sha256(fp))` | 32B content_hash | reverse fingerprint index |
| `i:` | `i:%04x` bucket | concatenated 32B hashes | chromaprint inverted index |
| `p:` | `hex(tx_hash)` | raw tx bytes | mempool (pending txs) |
| `sm:` | `hex(content_hash)` | `\0`-separated text | song metadata |
| `bh:` | `hex(content_hash)` | u32 | content → height (streaming) |
| `ia:` / `ig:` | lowercased artist / genre | packed 32B hashes | search indexes |
| `mlvl:` | `hex(addr)` | 1B `ModLevel` | on-chain mod level |
| `mpub:` | `hex(addr)` | 33B pubkey | mod pubkey (offline verify) |
| `mact:` | `hex(addr)` | u32 | mod "active since" height |
| `m:` | `hex(addr)` | — | legacy mod membership mirror |
| `founder:` | — | 20B addr | founder sentinel |
| `slashed:` | `hex(addr)` | `{1}` | slashing marker (§11) |
| `label:` / `art_label:` | name / `hex(artist)` | splits / label name | record-label payouts |
| `un:` / `addrun:` | username / `hex(addr)` | addr / username | username registry |
| `prop:` / `propstatus:` / `propvote:` | hashes | proposal data | governance (§7.4) |
| `ha:` / `hb:` / `ht:` | artist/album/title | original-case value | category hide lists |
| `ml:` / `ms:` | ts-sortable / sig16 | mod-log payload | moderation gossip log |
| `d:` | `hex(content_hash)` | — | deleted-song marker |
| `sw:` | `hash ‖ peer_id` | member record | swarm index (§18) |
| `rc:` | `hex(mini_addr)` | u64 | relay credits (§19) |
| `v:` | `hex(node_id)` | `ValidatorInfo` | validator registry — **dead** (`ValidatorRegistry` deleted, §9.6/§21 #9) |

Full details and serialization formats per prefix are in
[`database.cpp`](../src/storage/database.cpp); the line ranges are catalogued in the storage section of
the supporting research. `clear_derived_state()` wipes only `a: s: f: i: u:` (the
rebuildable-from-chain prefixes) for a replay ([`database.cpp:916-927`](../src/storage/database.cpp#L916)).

---

## 13. Network transport: librats wrapper

The live transport is **librats** (vendored, frozen at v0.2.0). The legacy libuv TCP mesh
was **deleted** in the Model 1 cleanup: `Peer` (`peer.{h,cpp}`) and the binary `Message`
format (`messages.{h,cpp}`) are gone, and **libuv was dropped from CMake** with them (§21 #9).
Only `NetworkManager` survives, as a slim shim ([`manager.h`](../src/network/manager.h),
[`manager.cpp`](../src/network/manager.cpp)) — it owns `NodeConfig`/`DhtEntry` and exposes
`peer_count` for status, but no longer carries any TCP transport.

### 13.1 Ports ⚠ partially vestigial

`NodeConfig` ([`manager.h:16-46`](../src/network/manager.h#L16)): `rats_port = 8080` is the **only**
port with a live listener (librats TCP/UDP); `p2p_port = 9333` and `api_port = 9334` are
retained for config-file compatibility but nothing opens a socket on them
([`manager.h:18-22`](../src/network/manager.h#L18)). The HTTP/3 surface (if built) runs on UDP/443.

### 13.2 RatsLink lifecycle

`RatsLink` ([`rats_link.cpp:37-119`](../src/network/rats_link.cpp#L37)) owns the librats client:
`rats_create` → `rats_start` → fetch our 40-hex peer id → dial bootstrap VPSes →
`rats_start_automatic_peer_discovery` → subscribe to the routes topic → register two typed
message dispatchers → spawn the route-broadcast thread. Bootstrap list comes from
`MUSICCHAIN_VPS_BOOTSTRAP`, defaulting to `127.0.0.1:8080` then the hardcoded VPS
`85.239.238.226:8080` ([`rats_link.cpp:306-341`](../src/network/rats_link.cpp#L306)). The loopback-first
entry exists for the colocated full-node + mini-node case (librats blocks dials to its own
public IPs but loopback's block is same-port only).

### 13.3 The three message channels over one client

Everything rides one librats client via three logical channels:

1. **RPC** — `musicchain.request` / `musicchain.reply`. Envelope
   `{req_id, type, body, originator_peer_id?}` ([`rats_api.cpp:44-46`](../src/api/rats_api.cpp#L44)).
   `RatsApi` borrows the client and registers `on_request_cb`. Replies are
   `{req_id, status, body|error}` where `status` maps HTTP-style codes to `"ok"` /error.
   The optional `originator_peer_id` is attached by a mini-node when it relays a player's
   envelope, and drives both proof-of-life touches and relay-credit attribution (§19).
2. **Consensus** — ⚠ **REMOVED.** The `mc:block_candidate` / `mc:block_confirmation`
   broadcast channel and its receive trampolines were deleted with the producer/co-sign
   machinery in the Model 1 pass (§9 banner, §21 #3). There is no candidate/confirmation
   gossip anymore: blocks are built → `connect_block` → announced, and block sync now rides
   the **RPC path (channel 1) plus the DHT** only, driven by `BlockPropagator`
   ([`block_propagator.cpp`](../src/sync/block_propagator.cpp)).
3. **Routes** — `musicchain.routes` topic. `build_route_json` ([`rats_link.cpp:148-172`](../src/network/rats_link.cpp#L148))
   advertises `{node_id, rats_peer_id, public_address, api_port, load_score, cpu_load,
   net_bps, is_busy, ts}`. The route loop ([`rats_link.cpp:343-414`](../src/network/rats_link.cpp#L343))
   ticks every second: redial the VPS if no validated peer, republish immediately when the
   link comes up or the peer count grows, and steady-state republish every 5 minutes.

### 13.4 UPnP & registry announcer ⚠ both gone

`UpnpMapper` ([`upnp.cpp`](../src/network/upnp.cpp)) still sits on disk but is **not compiled
into any target** — it is absent from `MC_SOURCES` and constructed nowhere, so it is dead
code. UPnP port-mapping was removed; **NAT traversal is now librats's job** (frozen at
v0.2.0): a player dials the mini-node outbound and inbound flows tunnel back through the
relay when reachability probing shows the node is firewalled (§6). *(A stale
`node_main.cpp` comment + a few `mc_rats_quic` references in `src/network`/`src/transport`
still name an old QUIC transport shim that no longer exists — librats itself is the
transport now.)*

The companion `HttpGossip` HTTP block-sync mesh (`registry_announcer.{h,cpp}`) was
**deleted** in the Model 1 cleanup, and libcurl was dropped from CMake with it (§21 #9);
block propagation now runs entirely over librats (`BlockPropagator`).

---

## 14. Block propagation protocol

`BlockPropagator` ([`block_propagator.cpp`](../src/sync/block_propagator.cpp)) is a Bitcoin-style block
distribution layer over librats, with BitTorrent-style DHT multi-source fetch for
catch-up. Every verb rides a `musicchain.request` envelope with `type` beginning `block.`,
dispatched into `BlockPropagator::handle_request` ([`rats_api.cpp:310-318`](../src/api/rats_api.cpp#L310)).

### 14.1 The five verbs

1. **`block.hello`** `{tip_height, tip_hash, timestamp_ms}` — exchanged both directions on
   connect. If the peer's tip is higher, immediately send `block.getblocks` with a
   locator; always reply with our own tip ([`block_propagator.cpp:232-260`](../src/sync/block_propagator.cpp#L232)).
2. **`block.getblocks`** `{locator: [...]}` — a Bitcoin block locator (tip, tip-1, tip-2,
   tip-4, …, genesis). The receiver finds the fork point and returns up to
   `kMaxInvCount = 32` hashes after it — delivered back as a `block.inv` (not a reply,
   because the reply lane is single-shot) ([`block_propagator.cpp:262-290`](../src/sync/block_propagator.cpp#L262)).
3. **`block.inv`** `{hashes: [...]}` — announce. Skip hashes we already have, mark the rest
   in the sender's `known` set (loop suppression), append to `expected_sequence_`, and
   `schedule_getdata` each ([`block_propagator.cpp:292-327`](../src/sync/block_propagator.cpp#L292)).
4. **`block.getdata`** `{hashes: [...]}` — request bodies. The receiver serves up to 32 per
   call as separate `block.data` requests, with misses in `notfound`
   ([`block_propagator.cpp:329-365`](../src/sync/block_propagator.cpp#L329)).
5. **`block.data`** `{hash, bytes_b64}` — a single block body, sent as its own request so
   multiple blocks stream in parallel from different peers. Receipt clears the peer's
   in-flight entry and calls `ingest_block_bytes` ([`block_propagator.cpp:367-389`](../src/sync/block_propagator.cpp#L367)).

### 14.2 Inbound block validation order

This is the network-side equivalent of §6.2, and the exact order matters. In
`ingest_block_bytes` ([`block_propagator.cpp:456-515`](../src/sync/block_propagator.cpp#L456)):

1. **Deserialize** — fail ⇒ discard.
2. **Hash match** — `block.hash() != expected_hash` ⇒ discard (defends a SHA-1 DHT
   collision).
3. **`Block::validate()`** — intrinsic consistency (§3.6) ⇒ discard on fail.
4. **(Model 1) No confirmation check.** The block-level signature/quorum step here was
   **removed** — blocks carry no votes. Validity is the deterministic content check (step 3)
   plus the `prev_hash` link + `apply_transactions` (tx signatures) enforced when `apply_loop`
   hands the block to `connect_block`. Every node re-derives the same verdict.
5. Stash into `pending_blocks_` and ensure the hash is in `expected_sequence_`.

Then `apply_loop` ([`block_propagator.cpp:517-586`](../src/sync/block_propagator.cpp#L517)) drains
`expected_sequence_` **in order**, pulling the next expected hash only when its body is
present, checking `prev_hash == local tip hash`, then calling **`Chain::connect_block`**
(which re-runs the full §6 validation as the final authority). On success it gossips
onward via `announce_new_block`.

> (The old §9.4-vs-step-4 signing-hash discrepancy is moot now: there are no confirmations
> to verify on either side. Resolved by removing them, not by reconciling the two hashes.)

### 14.3 Convergence & resilience

A behind-the-tip node converges by: connect → hello exchange → getblocks(locator) →
inv(32 hashes) → schedule_getdata fanned across peers + DHT searches → block.data bodies →
in-order apply → and, if peers are still ahead, another getblocks for the next 32-block
batch ([`block_propagator.cpp:560-583`](../src/sync/block_propagator.cpp#L560)). Background threads keep it
healthy: `dht_announce_loop` (announce every block hash so any node can serve it),
`stall_loop` (re-issue getdata after a 20s stall to a different peer), and `seed_dial_loop`
(dial explicit `sync_seeds` every 30s before the DHT populates).

---

## 15. Chain replay & startup validation

`rebuild_derived_state` ([`chain.cpp:915-1068`](../src/core/chain.cpp#L915)) re-derives all state by
replaying every block from height 1 to tip. It first `clear_derived_state()` (wipes `a: s:
f: i: u:`), then for each block runs the **same five checks the candidate handler does**:

1. `block->validate()` — internal consistency.
2. `prev_hash == previous block's hash` — chain linkage.
3. **(Model 1) No confirmation quorum step.** Removed — blocks carry no votes. Replay
   verifies the deterministic content + history checks only.
4. **Chromaprint fuzzy uniqueness** against the partial index built so far (threshold
   0.55) — catches duplicate registrations that slipped past earlier checks.
5. `apply_transactions` — re-apply every tx (signature/nonce/balance).

**If any block fails, replay stops and rolls the tip back to the last good height**
([`chain.cpp:1054-1066`](../src/core/chain.cpp#L1054)); the bad blocks past that point are ignored until
re-fetched. This is the self-healing path that protects against a corrupted on-disk block
or one a malicious peer pushed past a weaker earlier check.

(The old signing-hash replay caveat is gone — there are no confirmations to verify. §21 #2
is fixed.)

---

## 16. Deep audit (anti-forgery)

`DeepAuditor` ([`deep_audit.cpp`](../src/sync/deep_audit.cpp)) closes the gap that block validation
cannot: `Block::validate` only proves `sha256(compressed_fingerprint) ==
header.fingerprint_hash` — it does **not** prove the audio bytes at `content_hash`
actually match that fingerprint. A producer could pin a valid fingerprint but upload
unrelated audio.

A background thread ([`deep_audit.cpp:68-112`](../src/sync/deep_audit.cpp#L68)) wakes every
`kAuditIntervalMs = 60s` and **randomly samples one block** (85% from the newest 1024,
15% from the whole chain). `audit_block` ([`deep_audit.cpp:31-66`](../src/sync/deep_audit.cpp#L31)):
locate the content-addressed audio locally, re-fingerprint it via FFmpeg + chromaprint,
decompress the in-block declared fingerprint, and compare similarity against
`kSlashThreshold = 0.50`.

✅ **On a detected forgery it now invalidates and gossips** — it no longer merely logs.
The auditor (1) marks the content deleted locally (`mark_song_deleted`) and (2) fires an
`on_forgery` callback → `RatsApi::publish_forgery_report`, emitting a **node-signed
`forgery_report`** over the moderation-log gossip channel; receivers corroborate by **K=2
independent reporters OR a local re-audit** before deleting, so no single node can censor
([`deep_audit.cpp`](../src/sync/deep_audit.cpp), [`rats_api.cpp`](../src/api/rats_api.cpp);
§11.3, §21 #4). It still does not build a `SlashTx` (EQUIVOCATION is moot under Model 1,
§21 #4). It is also a sampling audit, not a full re-scan, and silently skips (`nullopt`) any
block whose audio it can't fetch locally.

---

## 17. RPC API surface & chain-affecting verbs

`RatsApi::handle_request` ([`rats_api.cpp:231`](../src/api/rats_api.cpp#L231)) is a linear `if/else if`
dispatch on the envelope `type`. The whole chain is wrapped in a try/catch that turns
exceptions into `{status:"server_error"}`. There is **no HTTP/1.1 listener** — `HttpServer`
is a socket-less verb container; the only live HTTP listener is a read-only `eth_*`
JSON-RPC on port 8545 ([`jsonrpc_server.cpp`](../src/api/jsonrpc_server.cpp)) for exchange/scanner
integration (writes return error `-32601`).

### 17.1 Read-only verbs

`status`, `stun.observe`, `dht.peers`, `songs.list/get/search`,
`wallet.balance/nonce/escrow_balance`. `songs.list` injects the live `swarm_size` from the
SwarmIndex so the client can hide songs nobody is serving.

### 17.2 `username.register`

([`rats_api.cpp:380-434`](../src/api/rats_api.cpp#L380)): parse name/owner/pubkey/nonce/signature →
build `UsernameTx` → require pubkey 33B + signature 64B → `tx.verify_signature()` →
**`db_.put_pending_tx(...)`** (into mempool) → `candidates_.wake()`. The nonce/duplicate
checks happen later at block production/validation (§7.5), not here.

### 17.3 `fingerprint.submit`

([`rats_api.cpp:539-810`](../src/api/rats_api.cpp#L539)): the song-registration + swarm-join entry
point. The server **always re-hashes** the submitted compressed fingerprint with SHA-256
(ignoring any client-claimed hash) to keep block validation consistent. Match resolution:
exact fingerprint lookup → client-supplied content_hash fallback → fuzzy chromaprint
bucket probe (threshold 0.55). On a **match**, it's a swarm join: announce the peer and
DHT-announce the song. On **no match** with a full fingerprint blob and valid content_hash,
it builds a `PendingRegistration` and **`candidates_.enqueue_registration(...)`** — which is
what later becomes a song block (§10).

### 17.4 Session lifecycle → mint

The play-reward pipeline lives in `HttpServer::post_session_*`
([`server.cpp:465-892`](../src/api/server.cpp#L465)):

- **`session.start`**: validate content_hash + player address; if `play_count >= 10000`,
  compute the burn and **reject with 402 if the player can't afford it**; create an
  in-memory session (no chain write yet).
- **`session.heartbeat`**: rate-cap at 1/second (429 if faster); require a `position_ms`;
  clamp against song duration; append the sample.
- **`session.complete`**: the mint. Replay-guard (`is_session_used` ⇒ 400); compute
  `effective_ms` via union-of-position-ranges with anti-farming clamps; **gate 1**
  (heartbeat density ≥ max(6, duration/10000)); **gate 2** (coverage ≥ 50% of duration, or
  30s legacy); build and **node-sign** the `PlayProof`; `compute_mint_outputs` →
  `MintTx{burn_amount}`; **`chain_.apply_mint(...)` + `db_.write(batch)`**; only on a
  successful write mark the session completed (so a failed write is retryable).

### 17.5 `offline.play_proof.submit` — replay ✅ / heuristics ✅

([`rats_api.cpp`](../src/api/rats_api.cpp)): ECDSA-verify the bundle (pubkey must derive to the
claimed address; canonical-JSON-minus-signature is the preimage), then **replay** each
session through the same `session.start/heartbeat/complete` verbs so all the gates and the
`is_session_used` dedup are shared. **✅ Cross-restart replay protection now implemented**: a
resubmitted bundle is rejected via a persisted `obp:<sig_hex>` marker (the 64-byte signature
is a stable per-bundle key), set only after ≥1 session credits so legit retries still work.
**✅ Now implemented:** all 11 bot-detection heuristics are wired
([`rats_api.cpp`](../src/api/rats_api.cpp)) — 6 hard-reject (perfect-interval / monotonic-jump /
density / concurrency / device-churn / fresh-wallet-volume) and 4 soft-log (screen / network /
battery / BSSID), backed by a rolling `obp:hist:<addr>` index, plus structural device
attestation (the player ships a hardware-derived `attestation` inside the wallet-signed bundle;
`device_id = sha256(device_key)` keys the churn heuristic). See §21 #5 and §22.8. **⚠ Remaining:**
real platform trust roots (Play Integrity / DeviceCheck / TPM) behind the `DeviceAttestationVerifier`
seam, and real battery/BSSID telemetry.

### 17.6 mini.hello & relay-credit attribution

`mini.hello` ([`rats_api.cpp:458-475`](../src/api/rats_api.cpp#L458)) records the relaying mini-node's
wallet into `peer_to_wallet_[peer_id]` (best-effort, no signature). This binding is what
the relay-credit counter (§19) uses to attribute credits.

---

## 18. Swarm index (content availability)

`SwarmIndex` ([`swarm.cpp`](../src/store/swarm.cpp)) maps each canonical `content_hash` to the set of
peers serving *some* encoding of that song, with each member's local content_hash,
bitrate, format, and last-seen. Variant-aware: the same song at different bitrates pins to
one canonical hash via fuzzy chromaprint match, so there's no duplicate chain entry.

**Availability is connection-authoritative**: `peer_available_` returns true *only* if the
peer is in `online_peers_` ([`swarm.cpp:99-103`](../src/store/swarm.cpp#L99)) — there is no TTL
fallback in the visibility check (Discover shows "online files only"). The 20-minute TTL
(`kStaleAfterMs`) survives only as a safety net for half-open connections, applied only to
*offline* peers in `prune_stale`.

Three eviction tiers on disconnect: `mark_peer_offline` (hide but keep persisted state for
fast reconnect), `evict_peer` (hard-drop everything, used on the librats disconnect
callback so `stream.open` won't return a dead peer), and `drop_peer` (replace a peer's
whole claimed set on a digest miss). The `peer_digest` is SHA-256 over the peer's sorted
canonical hashes, letting `swarm.hello_digest` skip a full resync when the library is
unchanged. Persisted under `sw:<hash>:<peer_id>`.

---

## 19. Relay-credit economy

`RelayCreditTracker` ([`relay_credit_tracker.cpp`](../src/net/relay_credit_tracker.cpp)) counts relayed
binary deliveries per mini-node and periodically mints rewards.

**Counting** ([`rats_api.cpp:1498-1522`](../src/api/rats_api.cpp#L1498)): after dispatch, increment by 1
**iff** all hold — a tracker is plugged in, `type == "stream.open"` (the only credited
verb today), `originator_peer_id` is non-empty (it was relayed, not direct),
`reply.status == "ok"`, and the relaying peer's wallet resolves via `peer_to_wallet_`.
Credit is **per-stream, not per-byte** — a 10 MB song and a 100 MB FLAC mint the same
credit (a known limitation, [`ARCHITECTURE.md §8.1`](../ARCHITECTURE.md)).

**Persistence**: credits hit `rc:<addr>` in LevelDB immediately (survives restarts;
hydrated on construction). **Sweep** every `kSweepIntervalMs = 5 min`
([`relay_credit_tracker.cpp:69-136`](../src/net/relay_credit_tracker.cpp#L69)): atomically swap out the
in-memory map, and for each `(addr, count)` clamp at `kMaxCountPerTx = 1_000_000` (carrying
the overflow to the next sweep), then call the mint callback. The callback builds a
`RelayRewardTx`, signs it with the founder key, and pushes it to the mempool; the chain
validates it via `apply_relay_reward` (§7.6), which enforces the **same** 1M cap so the
tracker can never emit an unmineable tx.

### 19.1 Triangulated relay-reward attestation (#10) — IMPLEMENTED

> **Implemented across all three binaries** (player/Dart, mini-node, full node). Replaces the
> old "the full node trusts that a relayed `stream.open` returned `ok` and credits `n=1`"
> with a **three-party cross-check** so a mini-node can't over-claim and a player can't deny.
> Wire details below match the code; the per-leg byte/format invariants are exact.
>
> **As-built (the abstract design followed by the concrete wire format):**
> - **Broker mint.** On `stream.open` the full node mints a random 16-byte `delivery_id`,
>   writes `pd:<delivery_id_hex>` → `{ch, broker, created, mw, relayed, received, flags}`
>   (flags bit0 brokered), and returns `delivery_id` on the reply body
>   ([`rats_api.cpp` `mint_delivery`](../src/api/rats_api.cpp)).
> - **F-frame carries it.** The requesting player threads `delivery_id` into its per-peer
>   `stream.open`; the serving player stamps the 16 raw bytes into the relay F-frame prefix
>   (`'F'`(1) + target(40 hex) + **delivery_id(16)** + chunk) — `player_server.dart`. The
>   mini-node strips exactly `1+40+16`, charges the payload against its rate bucket, and
>   accumulates bytes per `delivery_id` (`mini_node.cpp on_relay_binary`).
> - **Mini reports (signed).** On idle, the reaper broadcasts a `relay.report` to every full
>   node it knows (`g_routes`) — only the broker holds the matching `pd:` row; the rest reply
>   `ignored`. Preimage: `"relay.report" || delivery_id(16) || bytes_relayed(u64 LE) ||
>   mini_wallet(20)`, signed with the mini's wallet key.
> - **Player receipts (signed).** After the fetch the requesting player sends `relay.receipt`
>   to the broker. Preimage: `"relay.receipt" || delivery_id(16) || content_hash(32) ||
>   bytes_received(u64 LE)`, signed with the player wallet (hashes internally; broker verifies
>   with `verify_data`).
> - **Credit on corroboration.** When `flags` shows brokered+reported+receipted, the broker
>   credits `min(relayed,received)` bytes × **10 internal units** (1 MC / 10 MB, clamped
>   pre-multiply against overflow), then deletes the `pd:` row (single-use ⇒ replay-proof).
>   Orphaned rows are TTL-pruned by the `RelayCreditTracker` sweep.

The three parties each independently report the same relayed delivery to the full node,
which only credits the mini-node when the reports corroborate:

1. **Full node already saw the request.** A player must call the full node to learn *where*
   a song is (`stream.open` / swarm resolution, §3), so the full node has an authoritative
   record that *a* delivery of `content_hash` to `player_address` via a given mini-node was
   set up. This is the anchor — neither the mini-node nor the player can invent a session
   the full node never brokered.
2. **Mini-node reports the relay.** After forwarding the bytes, the mini-node sends the full
   node a packet detailing what it relayed: `(content_hash, originator player, byte count,
   session ref)`. This is its *claim* for credit.
3. **Player confirms receipt.** The player sends the full node a matching packet:
   `(content_hash, mini-node, bytes received, session ref)`, signed with its wallet key.

The full node **credits the mini-node only when all three line up** — the brokered request
exists, the mini-node's claim matches it, and the player's signed confirmation matches the
byte count (within tolerance). Mismatch ⇒ no credit (and a fraud signal). This makes the
credit **per-byte** (the confirmed byte count, not per-stream) and removes the founder-only
trust into a per-delivery anchor: the *full node that brokered the stream* mints the
`delivery_id` and is the only party that can corroborate, because it's the one party that
provably saw the request. As built this is the `relay.report` (mini→full) and `relay.receipt`
(player→full) verbs, the `pd:` pending-delivery table keyed by `delivery_id`, and
`RelayCreditTracker::increment` moving from `n=1`-per-`stream.open` to byte-count-on-
corroboration in **internal units** (so `apply_relay_reward` credits the count directly).
(Supersedes §21 #6's per-stream/whitelist remainder and #10's founder-only issuer — though
the founder key still *signs* the resulting `RelayRewardTx` until Phase-3 decentralized
signing.)

---

## 20. Fork choice, checkpoints, reorg

### 20.1 Fork choice ✅ (cumulative audited plays)

`tip_is_better(candidate, current)` ([`chain.h`](../src/core/chain.h)) now compares, in order:
**(1) cumulative fork weight** — higher wins; **(2)** height; **(3)** `timestamp_ms`; **(4)**
bytewise-greater hash (deterministic tiebreaker so every node converges).

**Weight = cumulative audited plays = total `MintTx` count from genesis to tip.** Each
`MintTx` required a realtime, per-device-gated, audited play to exist (§17.4), so weight is
demand-backed *and* deterministic — every node counts the same mints. Heartbeat and
registration-only blocks add 0, so a fork padded with free heartbeats **cannot** out-weight
a chain carrying real plays (closes the old free-heartbeat height-pump). Tracked per block
under the `cw:` prefix and on `ChainTip.weight`; recovered in `load_tip` and recomputed in
`rebuild_derived_state` ([`chain.cpp`](../src/core/chain.cpp)). `tip_is_better` is now actually
*used* — the propagator's "is this peer ahead?" decision and reorg trigger both apply it
(§20.3).

### 20.2 Checkpoints ⚠ empty

`hardcoded_checkpoints()` ([`chain.h:34-41`](../src/core/chain.h#L34)) is the eclipse-defense hook: a
synced chain must contain each checkpoint's exact hash at its exact height. **The list is
still empty** (#7), so the eclipse defense reduces to per-block validation + peer diversity
until mainnet has run long enough to bake in trusted heights. Unchanged this pass.

### 20.3 Reorg ✅ (weight-driven, try-and-rollback)

`Chain::reorg_to_branch(fork_hash, fork_height, branch, err)` ([`chain.cpp`](../src/core/chain.cpp))
adopts a heavier branch: it validates the branch links from a fork point on our chain,
checks its cumulative weight strictly exceeds our tip's, rewrites the height→hash (`n:`),
block (`b:`), and weight (`cw:`) indexes to the branch, repoints `t:tip`, and
`rebuild_derived_state()` re-derives all balances/indexes deterministically from the new
canonical chain. **Try-and-rollback**: if the re-derived chain isn't actually heavier (a
branch block failed to apply, so rebuild rolled it back), the previous chain is restored —
old block bytes (`b:`/`h:`/`cw:` keyed by hash) are never overwritten, so only `n:`/`t:tip`
move and restore rewrites them back. `disconnect_block` also now maintains the `cw:` weight
index.

**The propagator drives it** ([`block_propagator.cpp`](../src/sync/block_propagator.cpp)): `block.hello`
carries `tip_weight`; "peer ahead" is now the fork-choice rule (heavier wins, height is the
tiebreak), so a heavier-but-shorter chain still pulls us. When `apply_loop` hits a block
that forks off our tip, it walks back through the pending buffer to assemble the branch from
its fork point and calls `reorg_to_branch`; non-heavier or incomplete branches are dropped.

> ⚠ **New + untested.** The reorg path and branch assembly are new and not yet exercised by
> an automated test or a real multi-node reorg. Rebuild-on-reorg is O(chain length) and
> briefly clears derived state under the chain mutex (a concurrent RPC read can momentarily
> see empty balances). Fine for a young chain; revisit for scale. See §21 #8.

---

## 21. Consolidated "nerfed" inventory

The honest list of machinery that is present but inert, stubbed, or self-defeating. Items
marked **✅ FIXED** were implemented in the Model 1 pass (see the status banner at the top);
the rest remain ⚠ deferred.

1. **✅ FIXED — On-chain transfers.** `TransferTx` now carries an inline `from_pubkey`;
   `verify_signature` cross-checks `address_from_pubkey(from_pubkey) == from_address` and
   calls `verify_ecdsa` ([`transaction.cpp`](../src/core/transaction.cpp)), exactly like every
   other signed tx. `post_transfer` ([`server.cpp`](../src/api/server.cpp)) now requires
   `from_pubkey`. The always-false `verify_ecdsa_from_address` is no longer used by live
   code (only the dead legacy HTTP moderator route still references it, flagged for deletion).
2. **✅ FIXED — Signing-hash mismatch.** Confirmations were removed from the hashed header
   (format v3), so `hash()`/`signing_hash()` collapsed into one (`signing_hash` deleted).
   The dual-hash drift no longer exists. ([`block.cpp`](../src/core/block.cpp))
3. **✅ FIXED — Quorum / validator registry.** The quorum/voter-set model is gone:
   `dynamic_quorum`, `MAX_CONFIRMATIONS`, `BlockCandidate`, and `add_confirmation` removed;
   consensus is verify-and-converge (§22). The inert `ValidatorRegistry` was deleted.
4. **✅ FIXED — Deep Auditor teeth + mesh gossip.** On a detected forgery the auditor
   invalidates the content locally (`mark_song_deleted`) AND publishes a **node-signed
   `forgery_report`** over the moderation-log gossip channel
   ([`deep_audit.cpp`](../src/sync/deep_audit.cpp), [`rats_api.cpp`](../src/api/rats_api.cpp)
   `publish_forgery_report`/`handle_forgery_report`). Receivers corroborate by **K=2
   independent reporters OR a local re-audit** before deleting (no single node can censor),
   it replays to late joiners via `mod.sync_since`, and a `fr:<ch>:<reporter>` tally backs
   the quorum. **⚠ Remaining:** under Model 1 there are no block-level votes (EQUIVOCATION
   moot); `apply_slash`/`slashed:` stay for the vestigial validator path.
5. **✅ FIXED — Offline-proof anti-abuse.** Cross-restart replay protection (`obp:<sig>`)
   PLUS all **11 bot heuristics implemented** ([`rats_api.cpp`](../src/api/rats_api.cpp),
   `offline.play_proof.submit`): 6 hard-rejecting (perfect-interval/monotonic-jump/density/
   concurrency/device-churn/fresh-wallet-volume), 4 soft-logging (screen/network/battery/
   BSSID, skip-on-sentinel for the client-stubbed ones), backed by a rolling `obp:hist:<addr>`
   index. A pluggable **`DeviceAttestationVerifier`** (default `AcceptAll`,
   [`device_attestation.h`](../src/api/device_attestation.h)) derives the `device_id` the churn
   heuristic keys on; real Play-Integrity/App-Attest verifiers swap in with no call-site
   change. **✅ Now also: structural device attestation landed.** The player builds a
   hardware-derived `attestation` object — desktop via native FFI
   ([`hw_fingerprint.cpp`](../src/util/hw_fingerprint.cpp): MAC + board/CPU/disk serials + OS
   + MachineGuid), Android via a Kotlin MethodChannel (ANDROID_ID + `Build.*`) — and ships it
   INSIDE the wallet-signed offline bundle + `session.start` (so the wallet binds the device).
   The verifier derives `device_id = sha256(device_key)` (hardware-bound, not a resettable
   random) and **records the level** under `dal:<device_id>` (accept-and-record tier).
   **⚠ Remaining (needs platform trust roots):** real battery/BSSID values + the *real*
   hardware-attested verifier (Play Integrity / DeviceCheck / TPM — the structural object is
   the seam it drops into) + an online-session per-device concurrency cap.
6. **✅ FIXED — Relay reward: supply cap + per-byte triangulation.** `apply_relay_reward`
   rejects any mint past `SUPPLY_CAP`, and credit is now **per corroborated byte**, not
   per-stream: the broker mints a `delivery_id` at `stream.open`, the mini-node reports relayed
   bytes (signed) and the player receipts received bytes (signed), and only the matching broker
   credits `min(relayed,received) × 10 internal units` (§19.1, #10). `RelayCreditTracker` now
   accumulates internal units (not `n=1`) and `apply_relay_reward` credits the count directly.
   ([`chain.cpp`](../src/core/chain.cpp), [`rats_api.cpp`](../src/api/rats_api.cpp),
   [`relay_credit_tracker.cpp`](../src/net/relay_credit_tracker.cpp), [`mini_node.cpp`](../tools/mini_node.cpp))
7. **✅ FIXED (mechanism) — Config-driven checkpoints.** `Chain` now seeds `checkpoints_`
   from `hardcoded_checkpoints()` and merges a `checkpoints` array from `config.json`
   (`add_config_checkpoints`), and **consults them** in `connect_block`, `reorg_to_branch`,
   and `rebuild_derived_state` ([`chain.cpp`](../src/core/chain.cpp), [`node_main.cpp`](../tools/node_main.cpp)).
   The eclipse gate is live; the *list* is still empty (no audited mainnet height yet), so an
   operator can now pin one via config without recompiling — populate-later, not a code gap.
8. **✅ FIXED — Demand-weighted fork choice + automatic reorg.** Fork weight = cumulative
   audited plays (`MintTx` count), tracked under `cw:` and on `ChainTip.weight`;
   `tip_is_better` now compares weight first (§20.1). `Chain::reorg_to_branch` adopts a
   heavier branch via try-and-rollback + `rebuild_derived_state`, and the propagator
   assembles fork branches and triggers it (§20.3). Free heartbeats add 0 weight, closing
   the height-pump. **⚠ New + untested** — no automated reorg test yet; rebuild-on-reorg is
   O(chain length). ([`chain.cpp`](../src/core/chain.cpp), [`block_propagator.cpp`](../src/sync/block_propagator.cpp))
9. **✅ FIXED — Legacy dead code deleted.** `ValidatorRegistry`, and the libuv TCP mesh
   (`peer.{h,cpp}`, `messages.{h,cpp}`) + `HttpGossip` (`registry_announcer.{h,cpp}`) are
   **deleted**, and **libuv + libcurl dropped from CMake** (their only consumers). `NetworkManager`
   stays as a slim shim (it owns `NodeConfig`/`DhtEntry` and `peer_count` for status). HTTP/3
   (`h3_server`, `cert_util`) remains behind the default-OFF `MC_WITH_H3` flag.
10. **✅ FIXED (corroboration) — Relay-reward triangulation built.** The
    player/mini-node/full-node **triangulation** (§19.1) is implemented across all three
    binaries: the full node that *brokered* the stream mints the `delivery_id` and is the only
    party that can corroborate, crediting per-byte only when the brokered request + mini-node's
    signed `relay.report` + player's signed `relay.receipt` line up. **⚠ Remaining:** the founder
    key still *signs* the resulting `RelayRewardTx` ([`chain.cpp`](../src/core/chain.cpp),
    `apply_relay_reward`) until Phase-3 decentralized signing — the corroboration is now
    per-delivery, but the issuing *signature* authority is still single-anchor.

---

## 22. Chosen architecture: proof-of-unique-song (vote-free deterministic consensus)

> **Status: CORE IMPLEMENTED (uncommitted), features deferred.** Decision of record: the
> chain runs **vote-free deterministic consensus** — nodes *verify and converge*, they do
> not *cast and tally votes*. The consensus/block-format core of this section (§22.4 block
> format, §22.2/§22.9 deterministic ingest, removal of the vote machinery) is now in code;
> see the status banner at the top for the file list. The remaining subsections (§22.5
> demand-weighted fork choice + reorg, §22.6 attestation/anti-farming, §22.7 auditor teeth)
> are **deferred features** — treat their "should / will" as TODO. The §22.8 checklist marks
> which items are done. None of this is build-verified in the authoring environment.
>
> This decision does **not** get written into [`ARCHITECTURE.md`](../ARCHITECTURE.md) until it
> ships: that file is contractually "what is actually on disk," so Model 1 lands there
> section-by-section as each checklist item (§22.8) is implemented, not before.

### 22.1 Thesis — Model 1: verify and converge, never vote

There is **no single block producer and no stored validator votes.** A block is canonical
because validation is a **deterministic function of `(block content + prior chain state)`**:
any node that has the block re-derives the same verdict from the same history.

The distinction that defines Model 1, and that this design commits to:

- **Uniqueness is *verified*, not *voted*.** Whether a fingerprint already exists on chain
  is an objective fact about history, not a 51/49 opinion. Every honest node holding the
  same history reaches the **same** answer — agreement is **100% by computation**, not a
  majority of ballots. "Majority agreement" is therefore *emergent convergence on the
  heaviest valid chain*, never a quorum tallied into the header.
- **The only genuinely consensual question is *ordering*** — which block wins height N when
  two valid unique songs arrive at once — and that is settled by the **deterministic
  fork-choice rule** (§22.5), not by a vote. The mesh provides no global order; fork weight
  does.
- **"Check uniqueness, then vote" is explicitly rejected.** That phrasing describes *today's*
  confirmation quorum (§9.4: validators check, broadcast a `Confirmation`, `dynamic_quorum`
  finalizes). The `Confirmation` **is** the vote — so "vote" and "stored confirmation" are
  the same object. Model 1 **removes** it (§22.4), because under deterministic re-derivation
  the vote conveys no information a recomputation doesn't, it reintroduces the §21 #2
  dual-hash bug, it needs a Sybil-anchored voter set, and it scales *worse* (O(N) vote
  gossip per block instead of O(1) propagate-and-recheck).

The security claim — "it's still proof of work, because a lot of work is required to game
it" — is **true with a precise scope**: the work that gates an attacker is not the
fingerprint CPU (cheap per unit, §22.3) but the **realtime, per-device, attested play**
required to extract value (§22.6). Uniqueness secures *integrity*; realtime plays +
attestation secure *scarcity*. Keep those two separate and the model holds; conflate them
and it leaks (§22.7).

### 22.2 How validation propagates across the mesh

1. **One full node can validate alone.** Deterministic checks (fingerprint hash, merkle,
   duplicate-fingerprint vs chain state, tx signatures) need no peers — a single node is a
   complete authority over its own chain. This is already true:
   `rebuild_derived_state` re-derives everything from disk with no network help
   ([`chain.cpp:915-1068`](../src/core/chain.cpp#L915)).
2. **As nodes join the librats mesh, they fetch the block via DHT** and re-run the same
   deterministic validation. The block-fetch machinery already exists — `block.getdata` /
   `block.data` over librats, plus per-block-hash DHT announce so any node can serve any
   block ([`block_propagator.cpp`](../src/sync/block_propagator.cpp), §14). A newcomer pulls block N,
   re-derives its validity from content, and independently confirms the fingerprint is
   unique against its own copy of the chain.
3. **Agreement is convergence, not tallying.** Because the check is deterministic, every
   honest node that fetches block N reaches the same verdict. The "majority agreeing" is
   observed as *the heaviest chain everyone converges on*, not as a quorum of signatures
   baked into the header.

**Required changes:** none to the fetch path (DHT + getdata already work). The change is
*removing* the confirmation-quorum gate from the accept path (§22.4) and *replacing* fork
weight (§22.5) so that "what everyone converges on" is well-defined.

### 22.3 Why it is "proof of work" — and the honest cost accounting

The work exists, but you must account for **per-unit vs aggregate** cost and **who pays**:

| | Per-unit (attacker's cost to make one fake) | Aggregate (honest network's cost at scale) |
|---|---|---|
| **Compute a chromaprint** (decode + fingerprint) | ~1–2 s CPU/track — *cheap* | 100M-track re-audit = thousands of cores if done frequently/redundantly — *expensive* |
| **Hash the fingerprint bytes** (commitment) | microseconds | microseconds × blocks — negligible |

Two consequences that shape the whole design:

- **Re-fingerprinting must never be a per-block-per-validator consensus step.** Its cost
  lands on *honest verifiers* and scales with `catalog × validators` — a self-DoS, and a
  cost asymmetry in the wrong direction (the attacker pays ~2 s once; the network would pay
  `N × 2 s` forever). So the audio↔fingerprint re-check stays a **sampled spot-check** (the
  Deep Auditor, 1 block/min, §16), while every block cheaply commits the fingerprint via
  `sha256(compressed_fingerprint)` (§3.6).
- **Per-unit fingerprint cost is too small to be the Sybil barrier by itself.** The real
  barrier to *extracting value* is the realtime play gate (§22.6), not the registration
  CPU.

So "proof of work" is accurate, but the load-bearing work is **realtime plays from
distinct attested devices**, with fingerprint-uniqueness as the integrity layer that
makes each unit of that work attributable to exactly one canonical song.

### 22.4 Block-format change: one immutability hash, no stored confirmations

Today the canonical `block.hash()` covers the confirmations vector, which forces a second
`signing_hash()` and creates the dual-hash drift bug (§21 #2). The design target:

- **Commit only the immutable core**: `hash( [fingerprint_bytes] ‖ [transaction_bytes] )`.
  For a heartbeat (no song in 5 min) the fingerprint length is zero, so the preimage is
  effectively `[0x00…] ‖ [tx_bytes]`. The editable song *tag* (title/artist/genre/album)
  lives **outside** this hash, so a moderator can fix a tag without touching tx history or
  the fingerprint — and can never rewrite either, because both are committed.
- **Drop stored confirmations** (or, the minimal-risk interim: keep them attached for
  transport but exclude them from `hash()`). Under deterministic validation they are
  redundant — every node re-derives validity — and removing them from the preimage
  collapses `hash()` and `signing_hash()` into one, deleting the §21 #2 bug outright.

**Required changes:**
- `BlockHeader::serialize` / `hash` ([`block.cpp:68-100`](../src/core/block.cpp#L68)): stop hashing
  `confirmations`; remove `signing_hash()` and all its call sites
  ([`block_propagator.cpp:486`](../src/sync/block_propagator.cpp#L486), [`chain.cpp:974`](../src/core/chain.cpp#L974),
  [`node_main.cpp:547`](../tools/node_main.cpp#L547)).
- `Block::validate` ([`block.cpp:243-266`](../src/core/block.cpp#L243)): keep the fingerprint-hash and
  merkle checks; treat tags as non-committed metadata.
- Remove the confirmation-quorum gate from `ingest_block_bytes`
  ([`block_propagator.cpp:480-498`](../src/sync/block_propagator.cpp#L480)) and from replay
  ([`chain.cpp:967-991`](../src/core/chain.cpp#L967)) — validity is now purely the deterministic content
  check plus fork weight.
- This obsoletes most of `CandidateManager`'s broadcast-and-wait path (§9.3); block
  production becomes "build, validate, announce" with no confirmation round-trip.

> **Caveat — the irony to respect:** the moment you reintroduce "majority of *distinct
> identities* must agree" as the rule (rather than "majority of nodes converge on the
> heaviest valid chain"), a late-joiner can only verify that if the attested votes are
> *stored* — i.e. confirmations come back. Vote-free only works while canonicality is
> recomputable from content + fork weight. Don't drift back into identity-counting without
> re-adding the storage you removed here.

### 22.5 Fork choice: demand-weighted, heartbeats excluded ✅ IMPLEMENTED

> **Implemented** as cumulative audited plays (`MintTx` count) + try-and-rollback reorg —
> see §20.1/§20.3 for the as-built behavior. The rationale below is the design that drove it.

The old rule weighted raw **block height**, and heartbeat blocks are free to produce — so an
attacker could out-height an honest chain with empty blocks at zero cost. Under
proof-of-unique-song the fork weight must track the
**scarce, demand-backed resource**, not block count:

- **Weight = cumulative audited content / plays, not block count.** Registrations are
  near-free (noise farms, §22.7), so they cannot be the weight. Realtime audited plays are
  demand-backed and device-bounded, so they can.
- **Heartbeat blocks contribute zero fork weight.** They keep timestamps fresh but must not
  be a height pump.
- **One-song-once makes content non-replayable across forks.** A fingerprint can appear
  only once ([`chain.cpp:843-846`](../src/core/chain.cpp#L843)), so an attacker *cannot* pad a competing
  fork with copies of existing songs — they'd be rejected as duplicates. To build a heavier
  fork they must supply *new* unique audited content, which is the cost. This non-replay
  property is what gives fingerprint-work its fork resistance (the role nonce-binding plays
  in PoW).

**These were the required changes — now all implemented** (kept as the rationale that drove
the as-built behavior; see §20.1/§20.3 and §21 #8):
- ✅ `tip_is_better` ([`chain.h:56-62`](../src/core/chain.h#L56)) now compares a weight function over
  audited content/plays first, with the running weight persisted per tip (`cw:` / `ChainTip.weight`).
- ✅ `has_song == false` (heartbeat) blocks contribute zero weight, closing the height pump.
- ✅ `Chain::reorg_to_branch` is the automatic reorg driver (try-and-rollback +
  `rebuild_derived_state`), triggered by the propagator when a heavier valid fork appears.

### 22.6 The two security axes — and where Sybil actually bites

Sybil is dangerous on exactly two surfaces; the catalog/content surface is Sybil-tolerant.

- **Axis A — reward integrity ("is this play real or farmed?").** Defended by the
  **realtime play gate** (≥50% coverage, heartbeat density, one stream per device,
  [`server.cpp:772-803`](../src/api/server.cpp#L772)) plus **device attestation**. The binding
  constraint is *devices, not CPU*: per device, minting is capped at roughly
  `86,400 s ÷ ~30 s ≈ 2,880` mints/day (a few thousand even when gamed with short tracks),
  so an attacker's throughput is `(attested devices) × ~2,880`. Make devices expensive
  (Play Integrity / Keystore attestation) and the farm dies.
- **Axis B — chain canonicality ("which fork is real?").** Defended by §22.5's
  demand-weighted, non-replayable fork choice. Identity-counting is *not* used here
  (see the §22.4 caveat).
- **Content surface — Sybil-tolerant.** Audio is content-addressed and self-verifying;
  fake players, fake peers, and a polluted catalog are UX/DoS annoyances, not theft.
- **No external value path (bridge removed).** MC is **internal-only**: there is no live way
  to redeem it for outside value. The former Base bridge (`mcCOIN.sol` + `base-bridge.md`)
  was **deleted** — its audit found a custodial single-key trust model with no replay map and
  no fraud window, i.e. a 1:1 forge/farm→liquid-ETH amplifier. With it gone, **Axis B
  (fork-to-cash) is no longer a money vector** (the worst case is corrupting an internal-only
  token, not minting ETH), and **Axis A (reward farming) drops from deploy-blocker to
  internal-economy hygiene.** The §5/§22 anti-farming work still protects fair MC
  distribution and "online files only" integrity, just at lower urgency. *If a bridge is ever
  revisited, Axis A becomes a hard gate again — re-read this section then.*

**Required changes** (now internal-economy hygiene, not external-value gates):
- ✅ The 11 bot heuristics + cross-restart replay protection are implemented (§17.5, §21 #5).
- ✅ **Structural device attestation** is implemented end-to-end: the player ships a
  hardware-derived `attestation` (desktop native FFI / Android Kotlin) inside the wallet-signed
  bundle + `session.start`; the verifier derives a hardware-bound `device_id` and records the
  level (§21 #5). The per-device churn/volume heuristics now key on a non-resettable id.
- ⚠ The *real* hardware-attested verifier (Play Integrity / DeviceCheck / TPM — drops into the
  existing `DeviceAttestationVerifier` seam with no client change) + an online-session
  per-device concurrency cap.

### 22.7 The noise-farm hole (what this does NOT solve on its own)

Uniqueness is free; *realness* is unmeasurable on-chain. An attacker can generate millions
of distinct noise/synthetic/AI files, each a valid unique fingerprint that re-fingerprints
to itself — passing both the duplicate check and the Deep Auditor. The chain can verify
*distinctness* but not *demand or authorship*. Therefore:

- **Registration must not earn or carry fork weight on its own** — only realtime audited
  *plays* do (§22.5, §22.6). Noise registers fine but earns nothing and adds no weight.
- **The Deep Auditor must actually reject**, not just log — ✅ now done
  ([`deep_audit.cpp`](../src/sync/deep_audit.cpp), §11.3/§16, §21 #4). On a forgery it invalidates
  the content locally (`mark_song_deleted`) and gossips a **node-signed `forgery_report`**
  (corroborated by K=2 reporters or a local re-audit before any node drops the song), so the
  audio↔fingerprint integrity check is load-bearing now, not merely advisory. (It still does
  not build a `SlashTx` — EQUIVOCATION is moot under Model 1.)
- **Residual non-earning risks**: chain bloat (millions of ~400 KB registrations — bound by
  block-production rate and/or a registration cost) and namespace griefing (near-duplicate
  pre-registration just under the 0.55 threshold). Neither is theft, but both want a
  registration cost or rate limit.

### 22.8 Implementation checklist (consolidated)

Ordered list; ✅ = done in the Model 1 pass, ⚠ = deferred feature.

1. ✅ **Block format**: confirmations removed from the hashed header; `signing_hash`
   deleted; `BLOCK_VERSION` 2 → 3 (§22.4). (Tags were already outside `block.hash()`, which
   is header-only.)
2. ✅ **Drop the confirmation-quorum gate** from accept + replay; block production simplified
   to build → connect → announce (§22.4).
3. ✅ **Fork choice**: demand-weighted by cumulative audited plays (`MintTx` count),
   heartbeats add 0 weight, plus an automatic try-and-rollback reorg driver wired into the
   propagator (§20.1/§20.3/§22.5). **⚠ new + untested** — no automated reorg test yet.
4. ✅ **Deep Auditor with teeth**: forgery now → `mark_song_deleted` (local content
   invalidation) **plus** a node-signed `forgery_report` gossiped over the mod-log channel,
   corroborated by K=2 reporters or a local re-audit before any node drops the song (§21 #4).
   EQUIVOCATION is moot under Model 1 (§11, §16, §22.7).
5. ✅ **Anti-farming on Axis A**: cross-restart offline-proof replay protection (`obp:<sig>`),
   all 11 bot heuristics implemented, AND **structural hardware-bound device attestation**
   (native desktop + Kotlin Android fingerprint, recorded level) so the churn/volume rules key
   on a non-resettable `device_id` (§21 #5). *Still deferred:* the real hardware-attested
   verifier + a per-device online-concurrency limit (§22.6).
6. ✅ **Bridge removed.** The Base bridge (`mcCOIN.sol` + `base-bridge.md`) was **deleted** —
   MC is internal-only, which severs the Axis-B fork-to-cash amplifier entirely (§22.6).
7. ✅ **Latent bugs subsumed**: dual-hash drift (§21 #2) gone with step 1; weak quorum /
   inert validator registry (§21 #3) gone with step 2; **free-heartbeat height pump closed
   by step 3** (heartbeats carry 0 fork weight). Also ✅ outside this section: transfers
   (§21 #1) and relay-reward supply cap (§21 #6).
8. ✅ **Mesh validation semantics** (§22.9): the reject-vs-buffer split was already built
   (in-order `apply_loop` buffer) and is preserved. *(Eclipse backstop via checkpoints =
   ⚠ deferred, #7; ordering-by-fork-choice depends on #3.)*

### 22.9 Validation semantics over the mesh

Deterministic reject-on-mismatch is the model, and the mesh *helps* the easy part: because
validation is local, an invalid block dies at the first honest node and never propagates —
no quorum, no coordination, just each node privately refusing to relay or store garbage
(the Bitcoin pattern). Adding nodes adds enforcers for free. But on a mesh, "history" is
**per-node and time-varying**, so the rule is **not** "reject anything that doesn't match my
current view." Three distinctions are load-bearing:

1. **Orphan ≠ invalid — reject permanent contradictions, buffer temporary ones.** With
   propagation delay you can receive block N before N-1; its `prev_hash` won't match your
   tip *yet*, but it isn't invalid, it's early. Hard-rejecting it would drop valid blocks
   during normal lag. Already handled: out-of-order blocks are buffered in
   `expected_sequence_` / `pending_blocks_` and applied in order once the parent lands
   ([`block_propagator.cpp:517-586`](../src/sync/block_propagator.cpp#L517)), and `validate_candidate`
   deliberately skips the prev_hash check ([`chain.cpp:854-871`](../src/core/chain.cpp#L854)).
   **Permanent** contradictions — duplicate fingerprint, bad signature, bad merkle — are
   rejected outright; **temporary** ones — missing parent — are buffered and retried.
2. **Eclipse — your "history" is only what your neighbors show you.** A mesh does not
   guarantee you see the real chain. An attacker who surrounds a node with their own peers
   can feed it an internally-consistent *fake* history, and reject-on-mismatch will accept
   blocks matching that fake history. Your rejection is only as trustworthy as your view.
   Backstop = hardcoded checkpoints + peer diversity ([`chain.h:34-41`](../src/core/chain.h#L34),
   currently empty, §20.2). The mesh makes propagation easy but does **not** by itself prove
   you're on the canonical chain.
3. **Concurrent registrations have no global order — fork choice settles it, not the mesh.**
   Two nodes in different parts of the mesh each accept a *different* unique-but-similar song
   at the same height; both are valid against their local history, so uniqueness diverges by
   partition (which one is "the duplicate" depends on arrival order). The mesh provides no
   global order. The deterministic fork-choice rule (§22.5) picks the winning fork after the
   fact; on the losing fork the song retroactively becomes a duplicate and drops. So
   uniqueness-rejection is deterministic *given an order*, and the order is a fork-choice
   outcome, never a property of the mesh.

**Net:** dropping clearly-invalid blocks is easy and mesh-native (mostly already built);
not over-rejecting valid-but-early blocks is also already handled (in-order buffer); but
eclipse-resistance and concurrent-registration ordering are **not** things the mesh gives
you — they need checkpoints/peer-diversity and a deterministic fork-choice rule
respectively, and cannot be reached by "reject on mismatch" alone.

When any item ships, move its description out of §22 (design target) and into the relevant
"what's on disk" section (1–20), and delete the matching §21 bullet — same discipline as
[`ARCHITECTURE.md`](../ARCHITECTURE.md).

---

*End of document. Edit freely — the structure (numbered sections, `file:line` anchors,
the ⚠ NERFED tags, the §21 inventory, and the §22 design target) is designed so a diff
against this baseline reads cleanly when you adjust the architecture.*
