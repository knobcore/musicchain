#pragma once
#include "block.h"
#include "transaction.h"
#include "../storage/database.h"
#include <optional>
#include <string>
#include <map>
#include <mutex>
#include <set>

namespace mc {

struct ChainTip {
    Hash256  hash;
    uint32_t height;
    // Timestamp of the tip header (ms since epoch). Used by the
    // fork-choice rule below to break ties when two competing chains
    // sit at the same height. Zero on a freshly-initialized chain.
    uint64_t timestamp_ms = 0;
};

// Hardcoded checkpoint baked into the binary. Any chain we sync from a
// peer MUST contain this exact block hash at this exact height — if not,
// the peer is feeding us a fork that branches before the checkpoint,
// which we refuse. Eclipse defense: even if every peer we connect to is
// the attacker, they can't forge a chain that re-uses our checkpoint hash
// without already having the matching block. Update on every audited
// release; older checkpoints can stay in the list (they all have to pass).
struct Checkpoint {
    uint32_t height;
    Hash256  hash;
};
inline std::vector<Checkpoint> hardcoded_checkpoints() {
    // Empty for now — populate with (audited_height, audited_hash)
    // values once mainnet has run long enough that we trust a height
    // is irreversible. Until then sync still works (vacuously satisfies
    // every checkpoint), but the eclipse defense is just the per-block
    // validation + peer-diversity gate below.
    return {};
}

// Fork-choice rule used by SyncManager when comparing a peer's tip to
// our own. "Better" means we should adopt theirs.
//
//   1. Longer chain wins (higher height).
//   2. On ties, newer block timestamp wins.
//   3. On both ties, hash-bytewise wins (deterministic tiebreaker so
//      every node converges on the same winner; prevents oscillation
//      between two equally-good chains).
//
// This is the simplest defensible rule we can ship without an economic
// stake model — Bitcoin uses cumulative work, we use cumulative height
// + recency. Equivalent under honest-majority assumption, and easy for
// peers to verify without holding the whole chain.
inline bool tip_is_better(const ChainTip& candidate, const ChainTip& current) {
    if (candidate.height != current.height)
        return candidate.height > current.height;
    if (candidate.timestamp_ms != current.timestamp_ms)
        return candidate.timestamp_ms > current.timestamp_ms;
    return candidate.hash > current.hash;
}

// Manages the canonical blockchain: connect/disconnect blocks, tip tracking,
// state derivation (balances, fingerprint index, song state).
class Chain {
public:
    explicit Chain(Database& db);

    // Initialize from database; rebuilds derived state if necessary.
    bool init();

    // Returns current tip (height and hash). Atomic snapshot under the
    // chain's internal mutex so producer + network threads see a
    // consistent (hash, height) pair.
    ChainTip tip() const {
        std::lock_guard<std::mutex> lk(mu_);
        return tip_;
    }

    // Connect a validated block to the chain.
    // Updates tip, balances, song state, fingerprint index atomically.
    bool connect_block(const Block& block);

    // Disconnect the most recent block (for reorg support).
    bool disconnect_block();

    // Retrieve block by hash.
    std::optional<Block> get_block(const Hash256& hash) const;

    // Retrieve block hash at given height.
    std::optional<Hash256> get_block_hash(uint32_t height) const;

    // Height of a block hash (or nullopt if unknown).
    std::optional<uint32_t> get_block_height(const Hash256& hash) const;

    // Full-block SHA256 stored at a given height (for peer checksum verification).
    std::optional<Hash256> get_block_full_hash(uint32_t height) const;

    // Validate a candidate block against current chain state.
    // Does NOT connect it.
    bool validate_block(const Block& block, std::string& error) const;

    // Fast yes/no — is `content_hash` already on chain? Used by the
    // producer's re-queue logic so a duplicate-song registration gets
    // dropped instead of looping forever.
    bool validate_block_quick_duplicate(const Hash256& content_hash) const;

    // Rebuild all derived state by replaying all blocks.
    bool rebuild_derived_state();

    // Apply a transfer transaction (verifies signature + nonce, updates balances + nonce).
    // Called from post_transfer API handler and apply_transactions().
    bool apply_transfer(const TransferTx& tx, leveldb::WriteBatch& batch);

    // Apply a mint transaction directly (credits outputs, updates song state).
    // Called from session_complete API handler and apply_transactions().
    bool apply_mint(const MintTx& mint, uint64_t play_count_before,
                    leveldb::WriteBatch& batch);

    // Apply a moderator op (GRANT/REVOKE). `height` is the chain
    // height the op is being applied at (== tip_.height + 1 when
    // called from connect_block) — recorded as the moderator's "active
    // since" so quorum logic in Phase 3 can ignore freshly-granted
    // seats.
    bool apply_moderator_op(const ModeratorOpTx& tx,
                            uint32_t height,
                            leveldb::WriteBatch& batch);

    // Apply a Phase-3 proposal or vote tx. Routes HIDE_CONTENT /
    // RELEASE_ESCROW / VOTE_YES sub-codes, tallies votes, and on
    // reaching majority quorum executes the action atomically within
    // the same block batch.
    bool apply_proposal(const ProposalTx& tx,
                        uint32_t height,
                        leveldb::WriteBatch& batch);

    // First-come-first-served username registration. Anyone signs for
    // their own address; chain checks (a) name well-formedness, (b)
    // not already taken, (c) nonce, (d) signature.
    bool apply_username_register(const UsernameTx& tx,
                                 leveldb::WriteBatch& batch);

    // Slashing — verifies the cryptographic claim inside the SlashTx
    // evidence (EquivocationProof or FingerprintForgeryProof), then
    // marks target_address as slashed in the validator registry. From
    // this point forward Confirmation messages signed by their key are
    // not counted toward block-finality quorum.
    bool apply_slash(const SlashTx& tx, leveldb::WriteBatch& batch);

    // Has this address been slashed? Used by the consensus path that
    // tallies confirmations — slashed addresses' votes return zero
    // weight. Reads the "slashed:" prefix in the DB.
    bool is_slashed(const Address& addr) const;

private:
    // Serializes every connect_block / disconnect_block / tip() read.
    // Producer thread + network thread + RPC thread can all call into
    // the chain concurrently; this mutex stops them tearing the tip_
    // updates and double-applying blocks.
    mutable std::mutex mu_;
    Database& db_;
    ChainTip  tip_{};

    // Votes recorded earlier in the *current* block but not yet
    // flushed to leveldb. We consult both this set and the persistent
    // votes when computing quorum, so a proposal and its first vote in
    // the same block see each other.
    std::map<Hash256, std::set<Address>> proposal_votes_in_block_;

    // Most-recently-applied nonce per address within the current block
    // (the value just written into the WriteBatch). Lets apply_*
    // functions handle "two txs from the same address in the same
    // block" without reading from the unflushed batch — the canonical
    // case is bootstrap, which emits GRANT (nonce 0) + UsernameTx
    // (nonce 1) for the same founder address back-to-back.
    std::map<Address, uint64_t> applied_nonce_in_block_;

    // Read the next-expected nonce for `addr` considering both the DB
    // and any nonces already advanced earlier in the current block.
    uint64_t next_expected_nonce(const Address& addr) const;
    void     record_applied_nonce(const Address& addr, uint64_t new_value);

    bool apply_transactions(const Block& block, uint32_t height,
                            leveldb::WriteBatch& batch);

    // Count YES votes for a proposal across both the on-disk record
    // and the in-block staging set, deduping addresses.
    size_t effective_vote_count(const Hash256& prop_hash) const;

    // Execute a HIDE/RELEASE proposal directly into `batch` and flip
    // its propstatus to EXECUTED. Returns false if the action itself
    // is invalid (e.g. release amount exceeds escrow balance).
    bool execute_proposal(const ProposalTx& prop,
                          const Hash256& prop_hash,
                          leveldb::WriteBatch& batch);

    bool load_tip();
};

} // namespace mc
