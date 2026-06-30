#pragma once
#include <cstdint>

#include "../core/block.h"
#include "../core/transaction.h"

#include <string>
#include <vector>

namespace mc {

// ============================================================
// Slashing — design + on-wire types
// ============================================================
//
// Problem space: today a colluding quorum of validators can sign an
// alternative valid-looking chain. Every sync-time check passes — sigs
// verify, hashes chain — but the result is a fork the honest network
// rejects. There's currently no on-chain way to make those validators
// pay for that behaviour.
//
// Goal: every form of cheating a validator can do leaves cryptographic
// evidence on-chain that anyone can verify and that automatically
// reduces (slashes) the offender's confirmation weight to zero.
//
// Slashing rules:
//
// EQUIVOCATION
//   Validator signs two distinct blocks at the same height. The
//   `EquivocationProof` carries both Confirmation records — same
//   validator_id, same height, different block_hash. Anyone who has
//   seen both blocks can build the proof; the chain verifies the two
//   signatures and that the heights actually conflict.
//
// FINGERPRINT FORGERY  (from DeepAuditor)
//   Producer publishes a block where the audio at content_hash, when
//   decoded and re-fingerprinted, does NOT match the declared
//   compressed_fingerprint. The `FingerprintForgeryProof` carries the
//   producer's signed block_hash + a transcript an independent node can
//   reproduce: audio sha256, declared fingerprint, recomputed
//   fingerprint, similarity score. Other validators re-fetch the audio
//   (by content_hash) and confirm the mismatch before accepting the
//   slash.
//
// DOUBLE-VOTE
//   Validator submits two contradictory Confirmation messages for the
//   same block (one passing, one failing). Both are signed; the
//   contradiction is its own proof.
//
// Effect of a slash:
//   - validator_registry entry zeroed: confirmation_weight = 0
//   - their existing weight contribution disappears at the slash block
//   - other validators stop counting their confirmations going forward
//   - the slash record is permanent — there's no rehabilitation path
//     in v1; an operator who got slashed has to spin up a new identity
//     and start over
//
// What this does NOT prevent
//   - a freshly created identity from acting maliciously *once*
//     (single-use Sybil attack). Mitigation lives one layer up:
//     confirmation_weight scales with songs-registered × age, so a
//     brand-new validator carries near-zero weight on day one.
//   - off-chain bribery or out-of-band coordination
//
// Wire format (proto-style placeholders — replace when finalising):
//
// EquivocationProof
//   uint8  kind            = 1
//   uint32 height
//   Confirmation conf_a    // for block_a at `height`
//   Confirmation conf_b    // for block_b at `height`, block_a ≠ block_b
//   Hash256 block_a_hash
//   Hash256 block_b_hash
//
// FingerprintForgeryProof
//   uint8  kind            = 2
//   Hash256 block_hash
//   Hash256 content_hash      // audio claimed in the block
//   Hash256 audio_sha256      // what we actually fetched
//   string  declared_fp       // base64
//   string  recomputed_fp     // base64
//   float   similarity        // the score we measured
//   Confirmation reporter_conf // who's raising the proof (so the
//                              //  chain can dedup reports)
//
// Both proofs are wrapped in a new SlashTx transaction type so they
// flow through the existing mempool / connect_block pipeline. Adding
// the tx type touches transaction.h + capi.cpp + the JSON-RPC shim
// (eth_-equivalent slashing inspection verbs) — a separate session.
// This header reserves the structures so future code knows where they
// belong.

struct EquivocationProof {
    uint32_t      height = 0;
    Confirmation  conf_a{};
    Confirmation  conf_b{};
    Hash256       block_a_hash{};
    Hash256       block_b_hash{};

    // Verify the cryptographic claim: both sigs valid, validator_ids
    // equal, heights match, block hashes differ. Does NOT check whether
    // either block was ever on a canonical chain — that's a separate
    // check at apply time.
    bool verify() const;
};

struct FingerprintForgeryProof {
    Hash256      block_hash{};
    Hash256      content_hash{};
    Hash256      audio_sha256{};
    std::string  declared_fp;
    std::string  recomputed_fp;
    float        similarity = 0.0f;
    Confirmation reporter_conf{};

    bool verify(/*Chain& chain, */) const;
};

} // namespace mc
