#pragma once
#include <cstdint>
#include "../core/block.h"
#include "../storage/database.h"
#include <leveldb/write_batch.h>

namespace mc {

// Token precision constants
static constexpr uint64_t TOKEN_DECIMALS         = 100000000ULL; // 1 token = 1e8 units
// Listener earns the full per-play reward (and the artist share routes
// to a moderator-releasable escrow) only while the song's cumulative
// play count is below this. Past it the artist share is direct, the
// listener earns nothing, and the listener instead burns a dynamic
// amount that scales with total supply (see compute_burn_rate below).
static constexpr uint64_t FULL_REWARD_THRESHOLD  = 10000ULL;

// Base per-play reward magnitudes. All three are "1 token" in the
// economic model. Used for both pre- and post-10k tiers — only the
// routing changes (escrow vs spendable, burn vs no-burn).
static constexpr uint64_t FULL_ARTIST_REWARD     = 100000000ULL; // 1.00000000
static constexpr uint64_t FULL_NODE_REWARD       = 100000000ULL;
static constexpr uint64_t FULL_DISCOVERER_REWARD = 100000000ULL;
// Per-stream lanes (PlayProof v2): the player that SERVED the bytes (seeder) and
// the mini-node that RELAYED the stream each earn 1 token per qualifying play.
// The mini-node lane replaces the old per-byte RelayRewardTx with a flat
// per-stream credit.
static constexpr uint64_t FULL_SEEDER_REWARD     = 100000000ULL; // 1.00000000
static constexpr uint64_t FULL_MININODE_REWARD   = 100000000ULL; // 1.00000000

// Inflation control:
//   * SUPPLY_FLOOR — total minted at or below this triggers ZERO listener
//     burn; the chain is in "give the network away" mode.
//   * SUPPLY_CAP   — hard ceiling. apply_mint refuses to credit if the
//     post-mint total would exceed this. Reaching it freezes new mints.
// compute_burn_rate maps (supply - FLOOR) into a fast-growing burn
// amount, so usage gets exponentially more expensive as the chain
// approaches the cap and circulating supply devalues each token.
static constexpr uint64_t SUPPLY_FLOOR = 1'000'000'000ULL * 100000000ULL; // 1B tokens
static constexpr uint64_t SUPPLY_CAP   = 2'000'000'000ULL * 100000000ULL; // 2B tokens

/// Burn rate (in internal token units) charged to the listener wallet on
/// a single post-threshold play, given the chain's current total minted
/// supply.
///
/// * supply <  SUPPLY_FLOOR    → 0           (free network during bootstrap)
/// * supply >= SUPPLY_CAP      → UINT64_MAX  (apply_mint refuses — chain frozen)
/// * SUPPLY_FLOOR ≤ supply < SUPPLY_CAP →
///       pct = (supply - FLOOR) / (CAP - FLOOR)         ∈ [0, 1)
///       burn = pct^3 * 1000 tokens
///       i.e. roughly: 1B = 0 tokens, 1.5B = 125 tokens,
///                     1.8B = 512 tokens, 1.9B = 729 tokens.
///   The cubic curve makes burn grow much faster than supply does, so
///   the closer the chain gets to the cap the more aggressive the
///   "hyperdrive" deflation pressure becomes.
uint64_t compute_burn_rate(uint64_t total_supply);

struct AddressLess {
    bool operator()(const Address& a, const Address& b) const {
        return std::lexicographical_compare(
            a.begin(), a.end(), b.begin(), b.end());
    }
};

class Ledger {
public:
    explicit Ledger(Database& db);

    // Credit an address (minting). Reads/writes balance + total_supply
    // through the batch. NOT safe to call more than once for the SAME
    // address in the same batch — see credit_many for that case.
    void credit(leveldb::WriteBatch& batch, const Address& addr, uint64_t amount);

    // Credit a list of (address, amount) outputs in one shot. Pre-
    // aggregates per-address amounts so a mint with multiple outputs
    // to the same recipient composes correctly, and updates
    // total_supply with the FULL sum once. Use this from apply_mint
    // instead of the per-output credit loop the previous
    // implementation used.
    void credit_many(leveldb::WriteBatch& batch,
                     const std::vector<std::pair<Address, uint64_t>>& outs);

    // Debit an address; returns false if insufficient balance.
    // Zero-amount debit is a no-op that returns true.
    bool debit(leveldb::WriteBatch& batch, const Address& addr, uint64_t amount);

    // Transfer between addresses; returns false if insufficient
    // balance. Self-transfer is a no-op that returns true.
    bool transfer(leveldb::WriteBatch& batch,
                  const Address& from, const Address& to, uint64_t amount);

    // Query balance
    uint64_t balance(const Address& addr) const;

    // Format balance as decimal string with 8 decimal places
    static std::string format_balance(uint64_t internal_units);

    // Parse decimal string to internal units; returns false on error
    static bool parse_balance(const std::string& s, uint64_t& out);

private:
    Database& db_;
};

} // namespace mc
