#pragma once
#include <cstdint>
#include "../core/transaction.h"
#include "../storage/database.h"
#include "ledger.h"

namespace mc {

// Compute mint outputs for a given play proof.
// Uses current play_count from database to determine reward tier.
// Applies royalty splits to artist share.
std::vector<MintOutput> compute_mint_outputs(const PlayProof& proof,
                                              const SongSection& song,
                                              uint64_t play_count,
                                              const Hash256& serving_node_id,
                                              const Address& serving_node_address);

// Validate a mint transaction before inclusion:
//  - signature check
//  - session not already used
//  - duration check
//  - heartbeat count plausibility
bool validate_mint(const MintTx& mint, const Database& db, std::string& error);

} // namespace mc
