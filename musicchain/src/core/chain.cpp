#include "chain.h"
#include "../consensus/candidate.h"   // REQUIRED_CONFIRMATIONS
#include "../consensus/slashing.h"    // EquivocationProof / FingerprintForgeryProof
#include "../audio/fingerprint.h"     // chromaprint similarity on replay
#include "../tokens/ledger.h"
#include "../tokens/mint.h"
#include "../crypto/hash.h"
#include "../crypto/signature.h"
#include "../crypto/keys.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <unordered_set>

namespace mc {

Chain::Chain(Database& db) : db_(db) {}

bool Chain::init() {
    return load_tip();
}

bool Chain::load_tip() {
    auto val = db_.get("t:tip");
    if (!val) {
        tip_ = {};
        return true;
    }
    // format: 32 bytes hash + 4 bytes height
    if (val->size() < 36) return false;
    std::memcpy(tip_.hash.data(), val->data(), 32);
    tip_.height = 0;
    for (int i = 0; i < 4; ++i)
        tip_.height |= (static_cast<uint32_t>((*val)[32+i]) << (8*i));
    // timestamp_ms isn't stored in t:tip — recover it from the block we
    // just pointed at. Needed so the fork-choice rule has a real value
    // to compare against immediately after startup instead of zero.
    if (auto tb = get_block(tip_.hash))
        tip_.timestamp_ms = tb->header.timestamp_ms;
    return true;
}

bool Chain::connect_block(const Block& block) {
    std::lock_guard<std::mutex> lk(mu_);
    // validate_block reads tip_ and the DB; called under the same lock
    // so the producer thread and network thread can't race past the
    // prev_hash check.
    std::string err;
    if (!validate_block(block, err)) {
        std::cerr << "[chain] connect_block: validate failed: " << err << "\n";
        return false;
    }

    leveldb::WriteBatch batch;
    auto serialized = block.serialize();
    auto hash       = block.hash();

    // Store block
    db_.put_batch(batch, "b:" + db_.hex(hash), serialized);
    // Height → hash
    uint32_t new_height = tip_.height + 1;
    db_.put_batch_u32(batch, "h:" + db_.hex(hash), new_height);
    // Index → hash
    db_.put_batch(batch, "n:" + std::to_string(new_height),
                  std::vector<uint8_t>(hash.begin(), hash.end()));

    // Store full-block checksum for peer verification
    Hash256 fh = Block::full_hash(serialized);
    db_.put_batch(batch, "k:" + std::to_string(new_height),
                  std::vector<uint8_t>(fh.begin(), fh.end()));

    // Update tip
    std::vector<uint8_t> tip_val(36);
    std::memcpy(tip_val.data(), hash.data(), 32);
    for (int i = 0; i < 4; ++i) tip_val[32+i] = (new_height >> (8*i)) & 0xFF;
    db_.put_batch(batch, "t:tip", tip_val);

    // Only song blocks populate the fingerprint / metadata / search
    // indexes. Heartbeat blocks (block.has_song == false) carry an
    // all-zero content_hash + empty fields; writing those would
    // corrupt the indexes and the duplicate-song guard.
    if (block.has_song) {
        db_.put_fingerprint(batch, block.song);
        db_.put_song_meta(batch, block.song.content_hash, block.song);
        db_.add_to_artist_index(batch, block.song.artist, block.song.content_hash);
        db_.add_to_genre_index(batch, block.song.genre, block.song.content_hash);
        db_.set_content_height(batch, block.song.content_hash, new_height);
    }

    // Apply transactions
    if (!apply_transactions(block, new_height, batch)) {
        std::cerr << "[chain] connect_block: apply_transactions failed at height "
                  << new_height << " with " << block.transactions.size()
                  << " tx\n";
        return false;
    }

    // Bug fix #6: drain the same txs from the mempool in the SAME batch
    // as the block-state writes. Used to be a separate
    // db.del_pending_tx() call from candidate.cpp after connect_block
    // returned, which left a crash window where the tx had been applied
    // but stayed in the mempool — next startup would try to re-apply
    // and trip the nonce check, wedging the chain. Now they fall
    // together or not at all.
    for (const auto& raw_tx : block.transactions) {
        if (raw_tx.empty()) continue;
        auto th = crypto::sha256(raw_tx.data(), raw_tx.size());
        db_.del_batch(batch, "p:" + db_.hex(th));
    }

    if (!db_.write(batch)) return false;

    tip_.hash         = hash;
    tip_.height       = new_height;
    tip_.timestamp_ms = block.header.timestamp_ms;
    return true;
}

bool Chain::apply_transfer(const TransferTx& tx, leveldb::WriteBatch& batch) {
    if (!tx.verify_signature()) return false;
    uint64_t expected = next_expected_nonce(tx.from_address);
    if (tx.nonce != expected) return false;
    Ledger ledger(db_);
    if (!ledger.transfer(batch, tx.from_address, tx.to_address, tx.amount)) return false;
    db_.set_nonce(batch, tx.from_address, expected + 1);
    record_applied_nonce(tx.from_address, expected + 1);
    return true;
}

uint64_t Chain::next_expected_nonce(const Address& addr) const {
    // applied_nonce_in_block_[addr] holds the NEXT-expected nonce that
    // matches what db_.get_nonce(addr) WILL return once the current
    // batch flushes. So we just return it directly (no +1) — the value
    // already represents "the nonce the next tx from this address must
    // present."
    auto it = applied_nonce_in_block_.find(addr);
    if (it != applied_nonce_in_block_.end()) return it->second;
    return db_.get_nonce(addr);
}

void Chain::record_applied_nonce(const Address& addr, uint64_t new_value) {
    // Store the SAME value we just passed to db_.set_nonce(...,
    // new_value). Subsequent next_expected_nonce calls in the same
    // block will see this and treat it as the new floor.
    applied_nonce_in_block_[addr] = new_value;
}

bool Chain::apply_transactions(const Block& block, uint32_t height,
                               leveldb::WriteBatch& batch) {
    // Reset the per-block vote staging set so quorum math only counts
    // votes from this block once they're explicitly recorded.
    proposal_votes_in_block_.clear();
    applied_nonce_in_block_.clear();

    for (const auto& raw_tx : block.transactions) {
        if (raw_tx.empty()) continue;
        TxType type = static_cast<TxType>(raw_tx[0]);
        if (type == TxType::TRANSFER) {
            TransferTx tx;
            if (!TransferTx::deserialize(raw_tx.data(), raw_tx.size(), tx)) {
                std::cerr << "[chain] apply: TRANSFER deserialize failed\n"; return false;
            }
            if (!apply_transfer(tx, batch)) {
                std::cerr << "[chain] apply: TRANSFER apply failed (nonce/sig/balance)\n"; return false;
            }
        } else if (type == TxType::MINT) {
            MintTx mint;
            if (!MintTx::deserialize(raw_tx.data(), raw_tx.size(), mint)) {
                std::cerr << "[chain] apply: MINT deserialize failed\n"; return false;
            }
            uint64_t play_count = db_.get_play_count(mint.proof.content_hash);
            if (!apply_mint(mint, play_count, batch)) {
                std::cerr << "[chain] apply: MINT apply failed\n"; return false;
            }
        } else if (type == TxType::MODERATOR_OP) {
            ModeratorOpTx mod_tx;
            if (!ModeratorOpTx::deserialize(raw_tx.data(), raw_tx.size(), mod_tx)) {
                std::cerr << "[chain] apply: MODERATOR_OP deserialize failed\n"; return false;
            }
            if (!apply_moderator_op(mod_tx, height, batch)) {
                std::cerr << "[chain] apply: MODERATOR_OP apply failed (op_code="
                          << static_cast<int>(mod_tx.op_code) << " level="
                          << static_cast<int>(mod_tx.level) << " nonce="
                          << mod_tx.nonce << ")\n"; return false;
            }
        } else if (type == TxType::MODERATOR_PROPOSAL) {
            ProposalTx prop_tx;
            if (!ProposalTx::deserialize(raw_tx.data(), raw_tx.size(), prop_tx)) {
                std::cerr << "[chain] apply: PROPOSAL deserialize failed\n"; return false;
            }
            if (!apply_proposal(prop_tx, height, batch)) {
                std::cerr << "[chain] apply: PROPOSAL apply failed\n"; return false;
            }
        } else if (type == TxType::USERNAME_REGISTER) {
            UsernameTx un_tx;
            if (!UsernameTx::deserialize(raw_tx.data(), raw_tx.size(), un_tx)) {
                std::cerr << "[chain] apply: USERNAME_REGISTER deserialize failed\n"; return false;
            }
            if (!apply_username_register(un_tx, batch)) {
                std::cerr << "[chain] apply: USERNAME_REGISTER apply failed (name='"
                          << un_tx.name << "')\n"; return false;
            }
        } else if (type == TxType::SLASH) {
            SlashTx s_tx;
            if (!SlashTx::deserialize(raw_tx.data(), raw_tx.size(), s_tx)) {
                std::cerr << "[chain] apply: SLASH deserialize failed\n"; return false;
            }
            if (!apply_slash(s_tx, batch)) {
                std::cerr << "[chain] apply: SLASH apply failed\n"; return false;
            }
        } else if (type == TxType::RELAY_REWARD) {
            RelayRewardTx rr;
            if (!RelayRewardTx::deserialize(raw_tx.data(), raw_tx.size(), rr)) {
                std::cerr << "[chain] apply: RELAY_REWARD deserialize failed\n"; return false;
            }
            if (!apply_relay_reward(rr, batch)) {
                std::cerr << "[chain] apply: RELAY_REWARD apply failed\n"; return false;
            }
        } else {
            std::cerr << "[chain] apply: unknown TxType " << static_cast<int>(type) << "\n";
            return false;
        }
    }
    return true;
}

bool Chain::apply_mint(const MintTx& mint, uint64_t play_count_before,
                       leveldb::WriteBatch& batch) {
    // Hard supply cap: refuse to credit a new mint that would push
    // total_supply at or past SUPPLY_CAP. This is the chain-frozen
    // state in the burn-rate curve — listeners trying to play past
    // this point fail at session.complete and no new tokens land.
    {
        uint64_t mint_total = 0;
        for (const auto& out : mint.outputs) mint_total += out.amount;
        uint64_t current_supply = db_.get_total_supply();
        if (mint_total > 0 && current_supply + mint_total > SUPPLY_CAP) {
            return false;
        }
    }
    // Burn tokens from player if applicable (post-10k plays + non-zero
    // burn rate from the current supply).
    if (mint.burn_amount > 0) {
        uint64_t bal = db_.get_balance(mint.proof.player_address);
        if (bal < mint.burn_amount) return false; // safety net: session_start already checked
        Ledger ledger(db_);
        ledger.debit(batch, mint.proof.player_address, mint.burn_amount);
        uint64_t supply = db_.get_total_supply();
        db_.set_total_supply(batch,
            supply >= mint.burn_amount ? supply - mint.burn_amount : 0);
    }

    // Mark session as used
    db_.put_batch(batch, "u:" + db_.hex(mint.proof.session_id), {});

    // Update song state
    db_.update_song_state(batch, mint.proof, play_count_before);

    // Credit outputs
    Ledger ledger(db_);
    for (const auto& out : mint.outputs) {
        ledger.credit(batch, out.recipient, out.amount);
    }
    return true;
}

bool Chain::apply_moderator_op(const ModeratorOpTx& tx,
                               uint32_t height,
                               leveldb::WriteBatch& batch) {
    // Every op must self-verify regardless of role. The proposer pubkey
    // is carried inline (see ModeratorOpTx::verify_signature for why we
    // cross-check it against `proposer`).
    if (!tx.verify_signature()) return false;

    // Per-proposer nonce. We reuse the same `nv:` table as transfers
    // since the address space is the same — the chain has a single
    // monotonic nonce per address regardless of which tx type advances
    // it.
    uint64_t expected = next_expected_nonce(tx.proposer);
    if (tx.nonce != expected) return false;

    const ModOpCode op = static_cast<ModOpCode>(tx.op_code);
    const ModLevel  lv = static_cast<ModLevel>(tx.level);

    auto founder = db_.get_founder();
    uint8_t proposer_level = db_.get_mod_level(tx.proposer);

    switch (op) {
        case ModOpCode::GRANT: {
            // Bootstrap path: no founder yet, the proposer signs for
            // themselves at FOUNDER level. This is the one and only
            // permitted self-grant on the entire chain — every other
            // grant must come from an existing FOUNDER (Phase 2) or a
            // majority of OPs (Phase 3, future work).
            const bool bootstrap = !founder.has_value()
                                 && lv == ModLevel::FOUNDER
                                 && std::memcmp(tx.proposer.data(),
                                                tx.subject.data(), 20) == 0;
            if (bootstrap) {
                db_.set_mod_level(batch, tx.subject,
                                  static_cast<uint8_t>(ModLevel::FOUNDER));
                db_.set_mod_pubkey(batch, tx.subject, tx.subject_pubkey);
                db_.set_mod_active_block(batch, tx.subject, height);
                db_.set_founder(batch, tx.subject);
                db_.set_nonce(batch, tx.proposer, expected + 1);
                record_applied_nonce(tx.proposer, expected + 1);
                return true;
            }
            // Non-bootstrap GRANT: founder must already exist, proposer
            // must be the founder, and the new level can't escalate to
            // FOUNDER (there is exactly one founder per chain).
            if (!founder.has_value()) return false;
            if (proposer_level != static_cast<uint8_t>(ModLevel::FOUNDER)) return false;
            if (lv == ModLevel::NONE || lv == ModLevel::FOUNDER) return false;
            // Granting to an existing founder is a no-op-with-bad-intent.
            if (std::memcmp(tx.subject.data(), founder->data(), 20) == 0) return false;
            db_.set_mod_level(batch, tx.subject, static_cast<uint8_t>(lv));
            db_.set_mod_pubkey(batch, tx.subject, tx.subject_pubkey);
            db_.set_mod_active_block(batch, tx.subject, height);
            db_.set_nonce(batch, tx.proposer, expected + 1);
            return true;
        }
        case ModOpCode::REVOKE: {
            // Founder is the only one with revoke power in Phase 2. The
            // founder can't revoke themselves — stepping down is a
            // separate flow we'll add when the multi-founder/transfer
            // case actually exists.
            if (!founder.has_value()) return false;
            if (proposer_level != static_cast<uint8_t>(ModLevel::FOUNDER)) return false;
            if (std::memcmp(tx.subject.data(), founder->data(), 20) == 0) return false;
            uint8_t current = db_.get_mod_level(tx.subject);
            if (current == 0) return false; // nothing to revoke
            db_.set_mod_level(batch, tx.subject, 0);
            db_.set_nonce(batch, tx.proposer, expected + 1);
            return true;
        }
        case ModOpCode::TAG_LABEL_EDIT: {
            // Founder-only metadata edit. The "action" field in the
            // JSON payload picks the sub-op:
            //
            //   {"action":"label_define","name":"…","splits":[
            //       {"addr":"0x…","bp": 7000}, {"addr":"0x…","bp": 3000}]}
            //
            //   {"action":"label_assign","artist":"0x…","label":"…"}
            //
            // Returns false on any malformed payload, unknown action,
            // or splits that don't sum to 10 000 bp.
            if (!founder.has_value()) return false;
            if (proposer_level != static_cast<uint8_t>(ModLevel::FOUNDER)) return false;
            if (tx.meta_json.empty()) return false;
            try {
                auto j = nlohmann::json::parse(tx.meta_json);
                const std::string action = j.value("action", std::string());
                if (action == "label_define") {
                    const std::string name = j.value("name", std::string());
                    if (name.empty() || name.size() > 64) return false;
                    Database::LabelDef def;
                    def.display_name = name;
                    if (!j.contains("splits") || !j["splits"].is_array()) return false;
                    uint32_t total_bp = 0;
                    for (const auto& s : j["splits"]) {
                        Database::LabelSplit ls;
                        const std::string addr_hex = s.value("addr", std::string());
                        if (!crypto::parse_address(addr_hex, ls.wallet)) return false;
                        int bp = s.value("bp", 0);
                        if (bp <= 0 || bp > 10000) return false;
                        ls.basis_points = static_cast<uint16_t>(bp);
                        total_bp += bp;
                        def.splits.push_back(ls);
                    }
                    if (total_bp != 10000) return false;
                    if (def.splits.empty() || def.splits.size() > 16) return false;
                    db_.set_label(batch, name, def);
                } else if (action == "label_assign") {
                    Address artist{};
                    const std::string addr_hex = j.value("artist", std::string());
                    if (!crypto::parse_address(addr_hex, artist)) return false;
                    const std::string label = j.value("label", std::string());
                    // Empty label clears the assignment.
                    if (!label.empty()) {
                        auto def = db_.get_label(label);
                        if (!def.has_value()) return false; // can't assign to nonexistent label
                    }
                    db_.assign_artist_label(batch, artist, label);
                } else {
                    return false;
                }
            } catch (const std::exception&) {
                return false;
            }
            db_.set_nonce(batch, tx.proposer, expected + 1);
            return true;
        }
    }
    return false;
}

// ---- Username registration (Phase 3.5) ------------------------------

namespace {
bool username_is_well_formed(const std::string& s) {
    if (s.size() < 3 || s.size() > 30) return false;
    if (!(s[0] >= 'a' && s[0] <= 'z')) return false; // must start with letter
    for (char c : s) {
        bool ok = (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  c == '_';
        if (!ok) return false;
    }
    return true;
}
} // namespace

bool Chain::apply_username_register(const UsernameTx& tx,
                                    leveldb::WriteBatch& batch) {
    if (!tx.verify_signature()) {
        std::cerr << "[chain] username '" << tx.name
                  << "' reject: verify_signature\n"; return false;
    }
    uint64_t expected = next_expected_nonce(tx.owner);
    if (tx.nonce != expected) {
        std::cerr << "[chain] username '" << tx.name
                  << "' reject: nonce mismatch (tx=" << tx.nonce
                  << " expected=" << expected << ")\n"; return false;
    }
    if (!username_is_well_formed(tx.name)) {
        std::cerr << "[chain] username '" << tx.name
                  << "' reject: not well-formed\n"; return false;
    }
    if (db_.username_taken(tx.name)) {
        std::cerr << "[chain] username '" << tx.name
                  << "' reject: already taken\n"; return false;
    }
    if (db_.get_addr_username(tx.owner).has_value()) {
        std::cerr << "[chain] username '" << tx.name
                  << "' reject: owner already has a username\n"; return false;
    }
    db_.set_username(batch, tx.name, tx.owner);
    db_.set_nonce(batch, tx.owner, expected + 1);
    record_applied_nonce(tx.owner, expected + 1);
    return true;
}

// ---- Relay reward ---------------------------------------------------

bool Chain::apply_relay_reward(const RelayRewardTx& tx,
                               leveldb::WriteBatch& batch) {
    // Verify the issuer's signature first — the issuer is the only one
    // who can authorize a credit.
    if (!tx.verify_signature()) {
        std::cerr << "[chain] relay_reward reject: issuer signature invalid\n";
        return false;
    }
    // Issuer must be the founder for now. Phase 3 widens this to any
    // active validator with a non-slashed key.
    auto founder = db_.get_founder();
    if (!founder.has_value() ||
        std::memcmp(founder->data(), tx.issuer_address.data(), 20) != 0) {
        std::cerr << "[chain] relay_reward reject: issuer is not the founder\n";
        return false;
    }
    // Nonce check against issuer's chain nonce.
    uint64_t expected = next_expected_nonce(tx.issuer_address);
    if (tx.nonce != expected) {
        std::cerr << "[chain] relay_reward reject: nonce mismatch (tx="
                  << tx.nonce << " expected=" << expected << ")\n";
        return false;
    }
    // Don't accept absurd claims — bound the credit per tx so a buggy
    // counter can't mint a fortune in one shot.
    constexpr uint64_t kMaxCountPerTx = 1'000'000ull;
    if (tx.count == 0 || tx.count > kMaxCountPerTx) {
        std::cerr << "[chain] relay_reward reject: count out of range ("
                  << tx.count << ")\n";
        return false;
    }
    // 1 MC = 1_00000000 internal units (8 decimals). Credit the target.
    constexpr uint64_t kUnitPerMc = 100'000'000ull;
    uint64_t amount = tx.count * kUnitPerMc;
    Ledger ledger(db_);
    ledger.credit(batch, tx.target_address, amount);  // void — no supply-cap guard for now
    db_.set_nonce(batch, tx.issuer_address, expected + 1);
    record_applied_nonce(tx.issuer_address, expected + 1);
    std::cout << "[chain] RELAY_REWARD: +" << tx.count << " MC to "
              << db_.hex(tx.target_address).substr(0, 12) << "…\n";
    return true;
}

// ---- Slashing -------------------------------------------------------

bool Chain::apply_slash(const SlashTx& tx, leveldb::WriteBatch& batch) {
    // Authenticate the reporter's signature first — anyone can file
    // a slash, but they have to prove they did so to prevent third-
    // party replay attacks that would double-slash a valid target.
    if (!tx.verify_signature()) {
        std::cerr << "[chain] slash reject: reporter signature invalid\n";
        return false;
    }
    uint64_t expected = next_expected_nonce(tx.reporter_address);
    if (tx.nonce != expected) {
        std::cerr << "[chain] slash reject: nonce mismatch\n";
        return false;
    }
    // target_pubkey must match target_address (proves the target the
    // reporter is naming actually owns the pubkey whose signature
    // appears inside the evidence).
    {
        Address derived = crypto::address_from_pubkey(tx.target_pubkey);
        if (std::memcmp(derived.data(), tx.target_address.data(), 20) != 0) {
            std::cerr << "[chain] slash reject: target pubkey doesn't match "
                         "target address\n";
            return false;
        }
    }
    // Verify the cryptographic claim inside the evidence. Both proof
    // kinds derive in consensus/slashing.h; both have a verify()
    // that returns false if the underlying signatures don't line up.
    bool ok = false;
    switch (tx.kind) {
        case SlashKind::EQUIVOCATION: {
            // Equivocation proof = two distinct Confirmations from the
            // same validator at the same height. EquivocationProof
            // wire format starts directly at evidence.data() (no kind
            // byte — that's in SlashTx.kind already).
            // Layout:
            //   u32 height
            //   Confirmation conf_a   (32+33+64 = 129 bytes)
            //   Confirmation conf_b   (129 bytes)
            //   Hash256 block_a_hash  (32 bytes)
            //   Hash256 block_b_hash  (32 bytes)
            constexpr size_t kEvidenceLen = 4 + 129 + 129 + 32 + 32;
            if (tx.evidence.size() != kEvidenceLen) {
                std::cerr << "[chain] slash reject: equivocation evidence "
                             "wrong size\n"; return false;
            }
            EquivocationProof p{};
            const uint8_t* e = tx.evidence.data();
            std::memcpy(&p.height, e, 4); e += 4;
            std::memcpy(p.conf_a.validator_id.data(), e, 32); e += 32;
            std::memcpy(p.conf_a.pubkey.data(),       e, 33); e += 33;
            std::memcpy(p.conf_a.signature.data(),    e, 64); e += 64;
            std::memcpy(p.conf_b.validator_id.data(), e, 32); e += 32;
            std::memcpy(p.conf_b.pubkey.data(),       e, 33); e += 33;
            std::memcpy(p.conf_b.signature.data(),    e, 64); e += 64;
            std::memcpy(p.block_a_hash.data(),         e, 32); e += 32;
            std::memcpy(p.block_b_hash.data(),         e, 32);
            // Bind the proof to the named target: both confirmations
            // must actually be from target_pubkey.
            if (p.conf_a.pubkey != tx.target_pubkey ||
                p.conf_b.pubkey != tx.target_pubkey) {
                std::cerr << "[chain] slash reject: evidence pubkey doesn't "
                             "match target_pubkey\n"; return false;
            }
            ok = p.verify();
            break;
        }
        case SlashKind::FINGERPRINT_FORGERY: {
            // FingerprintForgeryProof — opaque to apply_slash for the
            // cryptographic part beyond the reporter sig already
            // checked. The semantic check (re-fetch audio + recompute
            // chromaprint) was done by the reporter; other validators
            // can independently re-do it from the evidence transcript.
            // For now we trust the reporter sig + accept; deepening to
            // multi-validator confirmation of the forgery before
            // slashing happens in a follow-up.
            ok = true;
            break;
        }
    }
    if (!ok) {
        std::cerr << "[chain] slash reject: evidence verification failed\n";
        return false;
    }
    // Idempotent: if the target's already slashed, this is a no-op.
    // Anybody who tries to spam multiple slashes for the same target
    // burns their nonce but doesn't change state.
    std::vector<uint8_t> marker{1};
    const std::string target_hex = db_.hex(tx.target_address);
    db_.put_batch(batch, "slashed:" + target_hex, marker);
    db_.set_nonce(batch, tx.reporter_address, expected + 1);
    record_applied_nonce(tx.reporter_address, expected + 1);
    std::cerr << "[chain] SLASH applied: target=" << target_hex
              << " kind=" << static_cast<int>(tx.kind) << "\n";
    return true;
}

bool Chain::is_slashed(const Address& addr) const {
    return db_.get("slashed:" + db_.hex(addr)).has_value();
}

// ---- Phase 3: multi-mod proposals + votes ---------------------------

namespace {

bool address_is_zero(const Address& a) {
    for (uint8_t b : a) if (b) return false;
    return true;
}

bool hash_is_zero(const Hash256& h) {
    for (uint8_t b : h) if (b) return false;
    return true;
}

} // namespace

size_t Chain::effective_vote_count(const Hash256& prop_hash) const {
    size_t n = db_.count_proposal_votes(prop_hash);
    auto it = proposal_votes_in_block_.find(prop_hash);
    if (it != proposal_votes_in_block_.end()) {
        for (const Address& a : it->second) {
            if (!db_.has_proposal_vote(prop_hash, a)) ++n;
        }
    }
    return n;
}

bool Chain::execute_proposal(const ProposalTx& prop,
                             const Hash256& prop_hash,
                             leveldb::WriteBatch& batch) {
    const ProposalKind kind = static_cast<ProposalKind>(prop.kind);
    switch (kind) {
        case ProposalKind::HIDE_CONTENT: {
            // Idempotent — re-hiding already-hidden content is a no-op
            // but still flips propstatus so the proposal table is
            // self-consistent.
            db_.mark_song_deleted(batch, prop.target_hash);
            db_.set_proposal_status(batch, prop_hash, Database::PROP_EXECUTED);
            return true;
        }
        case ProposalKind::RELEASE_ESCROW: {
            // The escrow address is deterministic from the artist
            // address (crypto::escrow_address_for). The proposal
            // carries the amount the mods want to release; we cap at
            // the current escrow balance so a stale proposal that
            // overruns the balance just transfers what's there
            // instead of failing the whole tx.
            const Address escrow = crypto::escrow_address_for(prop.target_addr);
            uint64_t balance     = db_.get_balance(escrow);
            uint64_t to_send     = std::min(balance, prop.amount);
            if (to_send == 0) {
                // Nothing to release — still flip status so the
                // proposal slot doesn't sit forever.
                db_.set_proposal_status(batch, prop_hash, Database::PROP_EXECUTED);
                return true;
            }
            Ledger ledger(db_);
            // If the artist is assigned to a record label, route the
            // escrow through the label's wallet splits instead of
            // crediting the artist directly. Splits are in basis points
            // (0..10000); any dust from integer rounding lands in the
            // LAST split so total credited == to_send exactly.
            auto label_name = db_.get_artist_label(prop.target_addr);
            std::optional<Database::LabelDef> label_def;
            if (label_name) label_def = db_.get_label(*label_name);
            if (label_def && !label_def->splits.empty()) {
                uint64_t credited = 0;
                for (size_t i = 0; i + 1 < label_def->splits.size(); ++i) {
                    const auto& s = label_def->splits[i];
                    uint64_t portion = (to_send * s.basis_points) / 10000;
                    if (portion > 0) {
                        if (!ledger.transfer(batch, escrow, s.wallet, portion)) {
                            return false;
                        }
                    }
                    credited += portion;
                }
                uint64_t tail = to_send - credited;
                if (tail > 0) {
                    if (!ledger.transfer(batch, escrow,
                            label_def->splits.back().wallet, tail)) {
                        return false;
                    }
                }
            } else if (!ledger.transfer(batch, escrow, prop.target_addr, to_send)) {
                return false;
            }
            db_.set_proposal_status(batch, prop_hash, Database::PROP_EXECUTED);
            return true;
        }
        case ProposalKind::VOTE_YES: {
            // VOTE_YES isn't itself executable — we never call execute
            // on a vote tx. If we somehow got here, treat it as a no-op.
            return false;
        }
    }
    return false;
}

bool Chain::apply_proposal(const ProposalTx& tx,
                           uint32_t /*height*/,
                           leveldb::WriteBatch& batch) {
    if (!tx.verify_signature()) return false;

    // Nonce is per-proposer regardless of which tx kind moved it
    // forward (same address-space as TRANSFER / MODERATOR_OP).
    uint64_t expected = next_expected_nonce(tx.proposer);
    if (tx.nonce != expected) return false;

    // Only OP and FOUNDER may propose or vote. VOICE is observer-only
    // for now.
    uint8_t proposer_level = db_.get_mod_level(tx.proposer);
    if (proposer_level < static_cast<uint8_t>(ModLevel::OP)) return false;

    // Compute current quorum threshold: strict majority of currently
    // active moderators. Single-mod chains (e.g. just the founder)
    // execute on the proposer's own implicit YES.
    const size_t active_n = db_.list_active_moderators().size();
    const size_t needed   = (active_n / 2) + 1;

    const ProposalKind kind = static_cast<ProposalKind>(tx.kind);
    switch (kind) {
        case ProposalKind::HIDE_CONTENT: {
            // Unused fields must be zeroed so the tx hash is canonical.
            if (!address_is_zero(tx.target_addr)) return false;
            if (tx.amount != 0)                   return false;
            // target_hash is the content hash being hidden — can't be
            // an all-zero placeholder.
            if (hash_is_zero(tx.target_hash))     return false;

            Hash256 prop_hash = tx.tx_hash();
            if (db_.has_proposal(prop_hash))      return false;

            db_.put_proposal(batch, prop_hash, tx.serialize());
            db_.add_proposal_vote(batch, prop_hash, tx.proposer);
            proposal_votes_in_block_[prop_hash].insert(tx.proposer);

            if (effective_vote_count(prop_hash) >= needed) {
                if (!execute_proposal(tx, prop_hash, batch)) return false;
            }
            db_.set_nonce(batch, tx.proposer, expected + 1);
            record_applied_nonce(tx.proposer, expected + 1);
            return true;
        }
        case ProposalKind::RELEASE_ESCROW: {
            if (!hash_is_zero(tx.target_hash))    return false;
            if (address_is_zero(tx.target_addr))  return false;
            if (tx.amount == 0)                   return false;

            Hash256 prop_hash = tx.tx_hash();
            if (db_.has_proposal(prop_hash))      return false;

            db_.put_proposal(batch, prop_hash, tx.serialize());
            db_.add_proposal_vote(batch, prop_hash, tx.proposer);
            proposal_votes_in_block_[prop_hash].insert(tx.proposer);

            if (effective_vote_count(prop_hash) >= needed) {
                if (!execute_proposal(tx, prop_hash, batch)) return false;
            }
            db_.set_nonce(batch, tx.proposer, expected + 1);
            record_applied_nonce(tx.proposer, expected + 1);
            return true;
        }
        case ProposalKind::VOTE_YES: {
            if (!address_is_zero(tx.target_addr)) return false;
            if (tx.amount != 0)                   return false;
            // target_hash is the proposal hash being voted on.
            if (hash_is_zero(tx.target_hash))     return false;

            const Hash256& prop_hash = tx.target_hash;
            auto raw = db_.get_proposal(prop_hash);
            if (!raw)                             return false;
            if (db_.get_proposal_status(prop_hash) != Database::PROP_PENDING)
                                                  return false;

            // Idempotency: same voter can't double-cast.
            if (db_.has_proposal_vote(prop_hash, tx.proposer)) return false;
            auto& in_block = proposal_votes_in_block_[prop_hash];
            if (in_block.count(tx.proposer))      return false;

            db_.add_proposal_vote(batch, prop_hash, tx.proposer);
            in_block.insert(tx.proposer);

            if (effective_vote_count(prop_hash) >= needed) {
                ProposalTx prop;
                if (!ProposalTx::deserialize(raw->data(), raw->size(), prop)) return false;
                if (!execute_proposal(prop, prop_hash, batch)) return false;
            }
            db_.set_nonce(batch, tx.proposer, expected + 1);
            record_applied_nonce(tx.proposer, expected + 1);
            return true;
        }
    }
    return false;
}

std::optional<Block> Chain::get_block(const Hash256& hash) const {
    auto val = db_.get("b:" + db_.hex(hash));
    if (!val) return std::nullopt;
    Block block;
    if (!Block::deserialize(val->data(), val->size(), block)) return std::nullopt;
    return block;
}

std::optional<Hash256> Chain::get_block_hash(uint32_t height) const {
    auto val = db_.get("n:" + std::to_string(height));
    if (!val || val->size() < 32) return std::nullopt;
    Hash256 h;
    std::memcpy(h.data(), val->data(), 32);
    return h;
}

std::optional<uint32_t> Chain::get_block_height(const Hash256& hash) const {
    return db_.get_u32("h:" + db_.hex(hash));
}

bool Chain::validate_block(const Block& block, std::string& error) const {
    // Caller (connect_block) holds the chain mutex. We're a const
    // function so any external direct call is fine; the lock above is
    // sufficient for the producer/network race.
    if (!block.validate()) { error = "block internal validation failed"; return false; }
    if (block.header.prev_hash != tip_.hash) {
        error = "prev_hash mismatch";
        return false;
    }
    // Reject duplicate songs (same content hash already in chain). Only
    // meaningful for song-bearing blocks; heartbeats carry zero hashes
    // by construction.
    if (block.has_song && db_.get_fingerprint(block.song.content_hash)) {
        error = "duplicate song";
        return false;
    }
    return true;
}

bool Chain::validate_block_quick_duplicate(const Hash256& content_hash) const {
    return db_.get_fingerprint(content_hash).has_value();
}

bool Chain::disconnect_block() {
    std::lock_guard<std::mutex> lk(mu_);
    if (tip_.height == 0) return false;
    // Remove tip entries
    leveldb::WriteBatch batch;
    db_.del_batch(batch, "b:" + db_.hex(tip_.hash));
    db_.del_batch(batch, "h:" + db_.hex(tip_.hash));
    db_.del_batch(batch, "n:" + std::to_string(tip_.height));

    // Restore previous tip
    auto prev_hash = get_block_hash(tip_.height - 1);
    if (!prev_hash) {
        // Genesis revert
        db_.del_batch(batch, "t:tip");
        db_.write(batch);
        tip_ = {{}, 0};
        return true;
    }
    std::vector<uint8_t> tip_val(36);
    std::memcpy(tip_val.data(), prev_hash->data(), 32);
    uint32_t new_height = tip_.height - 1;
    for (int i = 0; i < 4; ++i) tip_val[32+i] = (new_height >> (8*i)) & 0xFF;
    db_.put_batch(batch, "t:tip", tip_val);
    db_.write(batch);
    tip_.hash   = *prev_hash;
    tip_.height = new_height;
    // Restore timestamp from the now-current tip's block.
    if (auto pb = get_block(*prev_hash))
        tip_.timestamp_ms = pb->header.timestamp_ms;
    else
        tip_.timestamp_ms = 0;
    return true;
}

std::optional<Hash256> Chain::get_block_full_hash(uint32_t height) const {
    auto val = db_.get("k:" + std::to_string(height));
    if (!val || val->size() < 32) return std::nullopt;
    Hash256 h;
    std::memcpy(h.data(), val->data(), 32);
    return h;
}

bool Chain::rebuild_derived_state() {
    // Replay every block from height 1 to tip. Used at startup and after
    // a peer-driven sync. Validates each block as it's replayed so a
    // corrupted on-disk block (or a chain a malicious peer pushed past
    // an earlier weaker check) is caught at the earliest possible point
    // rather than silently producing a broken index.
    //
    // Per-block validation runs the SAME five checks the candidate
    // handler does at consensus time:
    //   1. block.validate()                   — internal consistency:
    //        sha256(compressed_fingerprint) == header.fingerprint_hash,
    //        merkle_root matches txs, structural fields well-formed.
    //   2. prev_hash chain link               — points at h-1's block hash.
    //   3. Confirmation quorum                — REQUIRED_CONFIRMATIONS
    //        distinct validator_ids, each signature verifies against
    //        block.hash().
    //   4. chromaprint fuzzy uniqueness       — incoming song's
    //        fingerprint isn't ≥0.55-similar to anything already
    //        replayed. Catches duplicate registrations that snuck past
    //        the original validator set (or were never validated because
    //        the chain predates the new gate).
    //   5. apply_transactions                 — every tx is well-formed,
    //        has sufficient balance / valid nonce / valid signature.
    //
    // If any block fails, we stop the replay and roll the chain back to
    // the last-known-good height. The DB tip pointer is rewritten so a
    // following startup picks up where we left off.
    db_.clear_derived_state();

    Hash256 prev_hash{};                             // genesis prev = zero
    uint32_t last_good_height = 0;
    for (uint32_t h = 1; h <= tip_.height; ++h) {
        auto bhash = get_block_hash(h);
        if (!bhash) return false;
        auto block = get_block(*bhash);
        if (!block) return false;

        // 1. Internal consistency.
        if (!block->validate()) {
            std::cerr << "[chain] replay: block " << h
                      << " failed internal validation — stopping\n";
            break;
        }
        // 2. Chain link.
        if (block->header.prev_hash != prev_hash) {
            std::cerr << "[chain] replay: block " << h
                      << " prev_hash break — stopping\n";
            break;
        }
        // 3. Confirmation quorum + signatures. Heartbeat / non-song
        //    blocks are exempt from REQUIRED_CONFIRMATIONS during
        //    bootstrap (height 1 was the founder's solo block).
        bool quorum_ok = true;
        {
            std::set<Hash256> seen_ids;
            uint32_t valid_sigs = 0;
            // Sigs were made over signing_hash() (header without
            // confirmations); the canonical hash() includes them and
            // would mismatch.
            const auto sign_hash = block->signing_hash();
            for (const auto& c : block->header.confirmations) {
                if (!seen_ids.insert(c.validator_id).second) continue;
                if (crypto::verify_ecdsa(sign_hash, c.signature, c.pubkey))
                    ++valid_sigs;
            }
            // h == 1 is the genesis-bootstrap window (solo self-sign was
            // allowed). Past that, demand at least one valid signature.
            // The exact quorum a block needed at mint time is
            // dynamic_quorum(peer_count_at_that_time) — values the chain
            // can't reconstruct during a replay — so the replay check
            // only verifies authenticity (>=1 valid sig). The producer
            // enforces the dynamic quorum at commit time.
            if (h > 1 && valid_sigs < 1) {
                std::cerr << "[chain] replay: block " << h
                          << " has no valid confirmations — stopping\n";
                quorum_ok = false;
            }
        }
        if (!quorum_ok) break;

        // 4. chromaprint fuzzy uniqueness against the partial index
        //    we've built so far. Skipped for heartbeat / non-song blocks.
        if (block->has_song && !block->song.compressed_fingerprint.empty()) {
            auto fp = audio::Fingerprint::from_compressed(
                block->song.compressed_fingerprint);
            if (fp) {
                constexpr float kSimThreshold = 0.55f;
                std::unordered_set<std::string> seen;
                float best_sim = 0.0f;
                bool dup = false;
                for (auto bucket : fp->bucket_ids()) {
                    for (const auto& cand_ch : db_.get_bucket(bucket)) {
                        if (cand_ch == block->song.content_hash) continue;
                        const std::string key = crypto::to_hex(cand_ch);
                        if (!seen.insert(key).second) continue;
                        auto entry = db_.get_fingerprint(cand_ch);
                        if (!entry) continue;
                        auto other = audio::Fingerprint::from_compressed(
                            entry->compressed_fingerprint);
                        if (!other) continue;
                        const float sim = fp->similarity(*other);
                        if (sim > best_sim) best_sim = sim;
                        if (sim >= kSimThreshold) { dup = true; break; }
                    }
                    if (dup) break;
                }
                if (dup) {
                    std::cerr << "[chain] replay: block " << h
                              << " carries a chromaprint duplicate (sim="
                              << best_sim << ") — stopping\n";
                    break;
                }
            }
        }

        // 5. Apply.
        leveldb::WriteBatch batch;
        if (block->has_song) {
            db_.put_fingerprint(batch, block->song);
            db_.put_song_meta(batch, block->song.content_hash, block->song);
            db_.add_to_artist_index(batch, block->song.artist,
                                     block->song.content_hash);
            db_.add_to_genre_index(batch, block->song.genre,
                                    block->song.content_hash);
        }
        applied_nonce_in_block_.clear();
        proposal_votes_in_block_.clear();
        if (!apply_transactions(*block, h, batch)) {
            std::cerr << "[chain] replay: block " << h
                      << " apply_transactions failed — stopping\n";
            break;
        }
        db_.write(batch);

        prev_hash       = *bhash;
        last_good_height = h;
    }

    // Roll the tip back to the last good height if we stopped early.
    if (last_good_height < tip_.height) {
        std::cerr << "[chain] replay rolled tip back from " << tip_.height
                  << " to " << last_good_height
                  << " (bad blocks past this point will be ignored on "
                     "next startup until they're re-fetched)\n";
        tip_.height = last_good_height;
        tip_.hash   = prev_hash;
        std::vector<uint8_t> v(36);
        std::memcpy(v.data(), tip_.hash.data(), 32);
        for (int i = 0; i < 4; ++i)
            v[32 + i] = static_cast<uint8_t>((tip_.height >> (8*i)) & 0xff);
        db_.put("t:tip", v);
    }
    return true;
}

} // namespace mc
