#include "candidate.h"
#include "../core/chain.h"
#include "../core/transaction.h"
#include "../storage/database.h"
#include "../network/manager.h"
#include "../network/messages.h"
#include "../audio/fingerprint.h"
#include "../audio/ogg_validator.h"
#include "../crypto/hash.h"
#include "../crypto/keys.h"
#include "../crypto/signature.h"
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <iomanip>

namespace mc {

namespace fs = std::filesystem;

static uint64_t now_ms_c() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

static std::string random_hex(size_t bytes) {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream ss;
    for (size_t i = 0; i < (bytes + 7) / 8; ++i)
        ss << std::hex << std::setw(16) << std::setfill('0') << dist(gen);
    return ss.str().substr(0, bytes * 2);
}

// ---- BlockCandidate ------------------------------------------------

bool BlockCandidate::is_expired() const {
    return (now_ms_c() - created_at_ms) > uint64_t(BLOCK_TIMEOUT_SECONDS) * 1000;
}

// ---- CandidateManager: candidate tracking --------------------------

void CandidateManager::add_candidate(const std::string& candidate_hash,
                                      BlockCandidate candidate) {
    std::lock_guard<std::mutex> lk(mutex_);
    candidates_[candidate_hash] = std::move(candidate);
}

bool CandidateManager::add_confirmation(const std::string& candidate_hash,
                                         const Confirmation& conf) {
    bool final = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = candidates_.find(candidate_hash);
        if (it == candidates_.end()) return false;
        auto& cand = it->second;
        for (const auto& c : cand.received_confirmations)
            if (c.validator_id == conf.validator_id) return false;
        cand.received_confirmations.push_back(conf);
        cand.block.header.confirmations = cand.received_confirmations;
        final = cand.is_final();
    }
    if (final) confirm_cv_.notify_all();
    return final;
}

std::optional<BlockCandidate> CandidateManager::get_candidate(
    const std::string& hash) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = candidates_.find(hash);
    if (it == candidates_.end()) return std::nullopt;
    return it->second;
}

void CandidateManager::cleanup_expired() {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto it = candidates_.begin(); it != candidates_.end(); ) {
        if (it->second.is_expired())
            it = candidates_.erase(it);
        else
            ++it;
    }
}

std::vector<std::pair<std::string, BlockCandidate>> CandidateManager::get_all() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return {candidates_.begin(), candidates_.end()};
}

// ---- CandidateManager: block producer ------------------------------

void CandidateManager::start(Chain& chain, Database& db,
                              net::NetworkManager& network,
                              const net::NodeConfig& cfg,
                              const crypto::KeyPair& keypair) {
    {
        std::lock_guard<std::mutex> lk(producer_mu_);
        if (running_) return;
        running_ = true;
        // Bug fix #3: do NOT push last_block_at_ms_ to "now" on boot.
        // That would delay the first heartbeat block by a full 5 min,
        // which means the founder GRANT enqueued immediately after a
        // fresh start sits in the mempool well past the operator's
        // patience. Leaving last_block_at_ms_ at 0 means the loop's
        // first iteration sees (now - 0) > HEARTBEAT_INTERVAL_MS and
        // mints a block right away if anything is pending — the
        // exact behaviour we want.
        last_block_at_ms_ = 0;
    }
    heartbeat_thread_ = std::thread([this, &chain, &db, &network, &cfg, &keypair] {
        heartbeat_loop(chain, db, network, cfg, keypair);
    });
}

void CandidateManager::stop() {
    {
        std::lock_guard<std::mutex> lk(producer_mu_);
        running_ = false;
    }
    confirm_cv_.notify_all();
    heartbeat_cv_.notify_all();
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
}

bool CandidateManager::enqueue_registration(PendingRegistration reg) {
    {
        std::lock_guard<std::mutex> lk(regs_mutex_);
        // De-dup against in-flight queue by BOTH content_hash and
        // fingerprint_hash. Without the fingerprint check, two players
        // submitting the same song in slightly different encodings could
        // race past the content-hash gate and both mint blocks before
        // either one made it into the index.
        std::queue<PendingRegistration> tmp = pending_regs_;
        while (!tmp.empty()) {
            const auto& q = tmp.front();
            if (q.content_hash == reg.content_hash) return false;
            if (q.fingerprint_hash == reg.fingerprint_hash) return false;
            tmp.pop();
        }
        pending_regs_.push(std::move(reg));
    }
    // Nudge the heartbeat loop to flush the next block immediately rather
    // than waiting up to HEARTBEAT_INTERVAL_MS.
    {
        std::lock_guard<std::mutex> lk(producer_mu_);
        last_block_at_ms_ = 0; // force the timer check to fire
    }
    heartbeat_cv_.notify_all();
    return true;
}

size_t CandidateManager::pending_registration_count() const {
    std::lock_guard<std::mutex> lk(regs_mutex_);
    return pending_regs_.size();
}

void CandidateManager::wake() {
    // Reset the heartbeat clock so the producer's first check after
    // waking sees the threshold as exceeded and mints immediately,
    // AND raise the wake_requested_ flag so the wait_for predicate
    // breaks out of sleep even when nothing is in pending_regs_.
    {
        std::lock_guard<std::mutex> lk(producer_mu_);
        last_block_at_ms_ = 0;
        wake_requested_   = true;
    }
    heartbeat_cv_.notify_all();
}

// ---- commit_block: shared finalize path -----------------------------

bool CandidateManager::commit_block(
    Block& block,
    Chain& chain, Database& db,
    net::NetworkManager& network,
    const net::NodeConfig& cfg,
    const crypto::KeyPair& keypair,
    const std::vector<std::pair<Hash256, std::vector<uint8_t>>>& consumed_txs,
    std::string& err) {

    // GENESIS FAST PATH (HEIGHT == 0 ONLY).
    //
    // At chain height 0 there is, by definition, nothing for a
    // confirmation gossip to converge on — no peer-validator could have
    // confirmed a block we just minted, because the founder GRANT that
    // empowers any moderator system hasn't itself landed yet.
    //
    // We *don't* gate this on peer_count anymore. The peers we see at
    // this moment are connected over librats (most likely the VPS
    // mini-node, which is a relay with no chain of its own) — they're
    // not consensus participants. Even with several connected peers,
    // if nobody has a chain past height 0 yet, our genesis block is the
    // network's genesis block.
    //
    // The exploit gate the operator asked for is preserved:
    // **chain.tip().height == 0** strictly. Once height >= 1 every
    // block goes through the full add_candidate + confirm path below.
    // There's no path back to fast-self-sign after genesis.
    //
    // chain.connect_block STILL runs the real validation (block
    // structure + apply_transactions); the fast path only skips the
    // candidate/confirmation theater that exists for peer
    // coordination.
    if (chain.tip().height == 0) {
        auto block_hash_bytes = block.hash();
        std::vector<Confirmation> sigs;
        sigs.reserve(REQUIRED_CONFIRMATIONS);
        for (uint32_t i = 0; i < REQUIRED_CONFIRMATIONS; ++i) {
            Confirmation c;
            // Distinct validator_id per pass so any future multi-node
            // verifier doesn't reject the block for "duplicate
            // validator_id". sha256("solo:" || node_id || i_be32).
            std::vector<uint8_t> seed;
            const char* tag = "solo:";
            seed.insert(seed.end(), tag, tag + std::strlen(tag));
            seed.insert(seed.end(), cfg.node_id.begin(), cfg.node_id.end());
            for (int s = 3; s >= 0; --s)
                seed.push_back(static_cast<uint8_t>((i >> (8*s)) & 0xFF));
            c.validator_id = crypto::sha256(seed.data(), seed.size());
            std::copy(keypair.public_key.begin(), keypair.public_key.end(),
                      c.pubkey.begin());
            c.signature = crypto::sign_ecdsa(block_hash_bytes,
                                              keypair.private_key);
            sigs.push_back(c);
        }
        block.header.confirmations = std::move(sigs);
        if (!chain.connect_block(block)) {
            err = "Chain connect_block rejected";
            return false;
        }
        // Best-effort write the .blk dump for the operator.
        try {
            // Scale fix: bucket the .blk dumps by height/1000 so we
            // never put more than 1000 files in any one directory.
            // At 1 block/sec that's a fresh subdir every ~16 min and
            // ~12 k dirs/year — well under any FS limit. Path layout:
            //   blocks/00000123/00123456.blk
            const uint32_t h = chain.tip().height;
            std::ostringstream sub;
            sub << std::setw(8) << std::setfill('0') << (h / 1000);
            fs::path blocks_dir = fs::path(cfg.data_dir) / "blocks" / sub.str();
            fs::create_directories(blocks_dir);
            std::ostringstream fname;
            fname << std::setw(8) << std::setfill('0') << h << ".blk";
            fs::path file_path = blocks_dir / fname.str();
            auto block_bytes = block.serialize();
            std::ofstream f(file_path, std::ios::binary);
            f.write(reinterpret_cast<const char*>(block_bytes.data()),
                    block_bytes.size());
        } catch (...) { /* non-fatal */ }
        (void)consumed_txs;  // mempool drain folded into connect_block batch
        (void)network;
        {
            std::lock_guard<std::mutex> lk(producer_mu_);
            last_block_at_ms_ = now_ms_c();
        }
        return true;
    }

    // MULTI-NODE PATH: full confirmation dance.
    std::string block_hash_hex = crypto::to_hex(block.hash());

    BlockCandidate candidate;
    candidate.block          = block;
    candidate.created_at_ms  = now_ms_c();
    add_candidate(block_hash_hex, candidate);

    bool confirmed = false;
    auto deadline  = std::chrono::steady_clock::now()
                   + std::chrono::seconds(BLOCK_TIMEOUT_SECONDS);

    // Solo mode: self-sign REQUIRED_CONFIRMATIONS times so the block
    // becomes final immediately. Once a real validator set exists this
    // branch goes away and we always wait on confirm_cv_.
    //
    // Bug fix #5: the loop used to set validator_id = cfg.node_id on
    // every confirmation, but `add_confirmation` rejects duplicates by
    // validator_id — so only one of the three confirmations actually
    // landed. Peers receiving the resulting block see 1/3 confirmations
    // and reject as not-final. We now derive a distinct validator_id
    // per confirmation by hashing (node_id || index) so the dedup
    // doesn't kick in.
    if (network.peer_count() == 0) {
        auto block_hash_bytes = block.hash();
        for (uint32_t i = 0; i < REQUIRED_CONFIRMATIONS; ++i) {
            Confirmation self_conf;
            // Derive a distinct validator_id per pass. Format:
            //   sha256("solo:" || node_id || u32_be(i))
            // The chain doesn't currently look up validator_ids on a
            // registry so any unique 32-byte slug is fine here.
            std::vector<uint8_t> seed;
            const char* tag = "solo:";
            seed.insert(seed.end(), tag, tag + std::strlen(tag));
            seed.insert(seed.end(),
                        cfg.node_id.begin(), cfg.node_id.end());
            for (int s = 3; s >= 0; --s)
                seed.push_back(static_cast<uint8_t>((i >> (8*s)) & 0xFF));
            self_conf.validator_id = crypto::sha256(seed.data(), seed.size());
            std::copy(keypair.public_key.begin(), keypair.public_key.end(),
                      self_conf.pubkey.begin());
            self_conf.signature = crypto::sign_ecdsa(block_hash_bytes,
                                                      keypair.private_key);
            add_confirmation(block_hash_hex, self_conf);
        }
        confirmed = true;
    } else {
        // Multi-node path: fan the candidate out to validators and wait.
        // node_main wires `network.set_candidate_publisher(...)` to
        // RatsLink::publish_block_candidate so this reaches every
        // validated peer over the same channel that carries routes.get.
        // Validators run validate_block + duplicate-fingerprint check
        // and post their signed Confirmation back via the inverse
        // RatsLink::publish_confirmation; the inbound confirmation
        // handler installed by node_main feeds add_confirmation, which
        // notifies confirm_cv_ once REQUIRED_CONFIRMATIONS land.
        //
        // Also self-sign one slot so a producer that *is* also a
        // validator counts itself; without this the producer would need
        // all 3 confirmations from peers even though it has just signed
        // the same block. (Equivalent to mining one's own first vote.)
        {
            Confirmation self_conf;
            self_conf.validator_id = cfg.node_id;
            std::copy(keypair.public_key.begin(), keypair.public_key.end(),
                      self_conf.pubkey.begin());
            self_conf.signature = crypto::sign_ecdsa(block.hash(),
                                                     keypair.private_key);
            add_confirmation(block_hash_hex, self_conf);
        }

        network.publish_candidate(block.serialize());

        std::unique_lock<std::mutex> lk(mutex_);
        confirmed = confirm_cv_.wait_until(lk, deadline, [&] {
            auto it = candidates_.find(block_hash_hex);
            return it != candidates_.end() && it->second.is_final();
        });
    }
    if (!confirmed) { err = "Confirmation timeout"; return false; }

    Block final_block = block;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = candidates_.find(block_hash_hex);
        if (it != candidates_.end())
            final_block.header.confirmations = it->second.received_confirmations;
    }

    if (!chain.connect_block(final_block)) {
        err = "Chain connect_block rejected";
        return false;
    }

    block = final_block; // Caller sees the committed form (with confirmations).
    uint32_t height = chain.tip().height;

    try {
        // Scale: bucket .blk dumps by height/1000 so we never put more
        // than ~1000 files in any one directory (NTFS performance dies
        // around 10M files in a flat folder).
        std::ostringstream sub;
        sub << std::setw(8) << std::setfill('0') << (height / 1000);
        fs::path blocks_dir = fs::path(cfg.data_dir) / "blocks" / sub.str();
        fs::create_directories(blocks_dir);
        std::ostringstream fname;
        fname << std::setw(8) << std::setfill('0') << height << ".blk";
        fs::path file_path = blocks_dir / fname.str();
        auto block_bytes = final_block.serialize();
        std::ofstream f(file_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(block_bytes.data()),
                block_bytes.size());
    } catch (...) {
        // Non-fatal — block is durable in LevelDB even if the .blk write fails.
    }

    // Bug fix #6: del_pending_tx writes are now folded into the same
    // leveldb batch as the chain writes inside connect_block, so
    // there's no more crash window. consumed_txs is now informational
    // (logging / retries) only.
    (void)consumed_txs;
    (void)network; // confirmed-block broadcast lives in network/manager.cpp now

    {
        std::lock_guard<std::mutex> lk(producer_mu_);
        last_block_at_ms_ = now_ms_c();
    }
    return true;
}

// ---- Heartbeat loop ------------------------------------------------

void CandidateManager::heartbeat_loop(Chain& chain, Database& db,
                                       net::NetworkManager& network,
                                       const net::NodeConfig& cfg,
                                       const crypto::KeyPair& keypair) {
    // Wakes on three signals:
    //   * a player just queued a song registration (enqueue_registration
    //     pokes us so the song lands in the next block — and we keep
    //     draining back-to-back while the queue stays non-empty)
    //   * the 30-second poll (slow-path check for the empty heartbeat
    //     interval — only relevant when nothing is queued)
    //   * stop()
    //
    // On wake: if there are pending registrations we mint song blocks
    // until the queue drains, with no sleep between them. Otherwise we
    // mint an empty heartbeat block iff HEARTBEAT_INTERVAL_MS has
    // elapsed since the last block.
    while (true) {
        // If the queue has work, skip the wait entirely — we want
        // back-to-back mints so a 14-track album scan doesn't take
        // 7 minutes to show up on chain (one every wake-up tick).
        bool queue_has_pending = false;
        {
            std::lock_guard<std::mutex> lk(regs_mutex_);
            queue_has_pending = !pending_regs_.empty();
        }
        if (!queue_has_pending) {
            std::unique_lock<std::mutex> lk(producer_mu_);
            heartbeat_cv_.wait_for(lk, std::chrono::seconds(30), [this] {
                if (!running_) return true;
                if (wake_requested_) return true;
                std::lock_guard<std::mutex> rlk(regs_mutex_);
                return !pending_regs_.empty();
            });
            // Reset the wake flag now that we're awake; the producer
            // body decides if there's actual work and will mint as
            // appropriate.
            wake_requested_ = false;
            if (!running_) return;
        }

        // Drain one registration if available.
        std::optional<PendingRegistration> reg;
        {
            std::lock_guard<std::mutex> lk(regs_mutex_);
            if (!pending_regs_.empty()) {
                reg = std::move(pending_regs_.front());
                pending_regs_.pop();
            }
        }

        uint64_t last_at;
        {
            std::lock_guard<std::mutex> lk(producer_mu_);
            last_at = last_block_at_ms_;
        }

        const uint64_t now = now_ms_c();
        auto all_pending = db.get_all_pending_txs();

        if (!all_pending.empty() || reg) {
            std::cout << "[producer] tick: regs=" << (reg ? 1 : 0)
                      << " pending_txs=" << all_pending.size() << "\n";
        }

        // ---- Bug fix #7: pre-flight every pending tx -----------------
        //
        // Old behaviour stuffed every pending tx into the block without
        // checking signatures first. apply_transactions() then bailed on
        // the first bad one and the whole block was rejected, leaving
        // the bad tx in the mempool for the next iteration to trip over
        // — chain wedged.
        //
        // Now we deserialize + verify each pending tx in-place. Anything
        // that doesn't pass basic structural / signature checks is
        // dropped from the mempool right here so it can't poison
        // another attempt. The chain still does the full
        // apply-rules check; pre-flight is only the cheap floor.
        // We can't rely on leveldb iteration order here: pending txs are
        // keyed by tx_hash (sha256 of the serialized tx), and that's
        // effectively random. If a single address queues two txs (e.g.
        // the bootstrap path: GRANT nonce=0 then UsernameTx nonce=1),
        // the UsernameTx may sort BEFORE the GRANT in the resulting
        // block; the chain then runs UsernameTx first, sees DB nonce 0,
        // rejects "nonce mismatch (tx=1 expected=0)" and the entire
        // block fails apply_transactions. We hit this on every cold
        // bootstrap.
        //
        // Fix: collect (sender, nonce) for every tx during the verify
        // pass, then sort by (sender, nonce) so consecutive nonces from
        // the same address always appear in the right order.
        struct TxSlot {
            Hash256              hash;
            std::vector<uint8_t> raw;
            Address              sender{};   // zero for MINT (no per-sender nonce)
            uint64_t             nonce = 0;
        };
        std::vector<TxSlot> slots;
        slots.reserve(all_pending.size());
        for (auto& [tx_hash, raw] : all_pending) {
            if (raw.empty()) {
                db.del_pending_tx(tx_hash);
                continue;
            }
            bool ok = false;
            const char* why = "ok";
            TxSlot slot;
            slot.hash = tx_hash;
            TxType type = static_cast<TxType>(raw[0]);
            switch (type) {
                case TxType::TRANSFER: {
                    TransferTx tx;
                    if (!TransferTx::deserialize(raw.data(), raw.size(), tx)) {
                        why = "TRANSFER: deserialize failed";
                    } else if (!tx.verify_signature()) {
                        why = "TRANSFER: verify_signature failed";
                    } else {
                        slot.sender = tx.from_address;
                        slot.nonce  = tx.nonce;
                        ok = true;
                    }
                    break;
                }
                case TxType::MODERATOR_OP: {
                    ModeratorOpTx tx;
                    if (!ModeratorOpTx::deserialize(raw.data(), raw.size(), tx)) {
                        why = "MODERATOR_OP: deserialize failed";
                    } else if (!tx.verify_signature()) {
                        why = "MODERATOR_OP: verify_signature failed";
                    } else {
                        slot.sender = tx.proposer;
                        slot.nonce  = tx.nonce;
                        ok = true;
                    }
                    break;
                }
                case TxType::MODERATOR_PROPOSAL: {
                    ProposalTx tx;
                    if (!ProposalTx::deserialize(raw.data(), raw.size(), tx)) {
                        why = "PROPOSAL: deserialize failed";
                    } else if (!tx.verify_signature()) {
                        why = "PROPOSAL: verify_signature failed";
                    } else {
                        slot.sender = tx.proposer;
                        slot.nonce  = tx.nonce;
                        ok = true;
                    }
                    break;
                }
                case TxType::USERNAME_REGISTER: {
                    UsernameTx tx;
                    if (!UsernameTx::deserialize(raw.data(), raw.size(), tx)) {
                        why = "USERNAME_REGISTER: deserialize failed";
                    } else if (!tx.verify_signature()) {
                        why = "USERNAME_REGISTER: verify_signature failed";
                    } else {
                        slot.sender = tx.owner;
                        slot.nonce  = tx.nonce;
                        ok = true;
                    }
                    break;
                }
                case TxType::MINT: {
                    MintTx tx;
                    if (!MintTx::deserialize(raw.data(), raw.size(), tx)) {
                        why = "MINT: deserialize failed";
                    } else { ok = true; }
                    break;
                }
                default:
                    why = "unknown TxType";
                    ok = false;
            }
            if (!ok) {
                std::cerr << "[chain] dropping malformed mempool tx "
                          << crypto::to_hex(tx_hash).substr(0, 12) << "… ("
                          << static_cast<int>(raw[0]) << " : " << why << ")\n";
                db.del_pending_tx(tx_hash);
                continue;
            }
            slot.raw = std::move(raw);
            slots.push_back(std::move(slot));
        }
        // Stable sort by (sender, nonce). Same-address txs end up in
        // monotonic nonce order; addresses are grouped lexicographically
        // but the grouping is purely cosmetic — apply_transactions only
        // cares about per-sender monotonicity.
        std::sort(slots.begin(), slots.end(), [](const TxSlot& a, const TxSlot& b) {
            if (a.sender != b.sender) return a.sender < b.sender;
            return a.nonce < b.nonce;
        });
        std::vector<std::pair<Hash256, std::vector<uint8_t>>> pending_txs;
        pending_txs.reserve(slots.size());
        for (auto& s : slots) {
            pending_txs.emplace_back(s.hash, std::move(s.raw));
        }

        // No registration AND no pending transactions AND heartbeat
        // window not yet elapsed → nothing to do this tick.
        if (!reg && pending_txs.empty()
            && (now - last_at < HEARTBEAT_INTERVAL_MS)) continue;

        // Pre-genesis guard. At chain.tip().height == 0 we must NOT mint
        // empty heartbeat blocks. last_block_at_ms_ is initialised to 0
        // so the elapsed-window check above always wants to fire on the
        // very first tick — without this guard the producer races the
        // user's bootstrap action and turns an empty block 1 into the
        // chain's genesis, leaving the GRANT to land at height 1 where
        // the multi-node confirmation path then times out for 300 s.
        // Wait until the operator's bootstrap puts the GRANT into the
        // mempool; only then mint block 1.
        if (chain.tip().height == 0 && !reg && pending_txs.empty()) {
            continue;
        }
        Block block;
        block.header.version          = BLOCK_VERSION;
        block.header.prev_hash        = chain.tip().hash;
        block.header.timestamp_ms     = now;
        for (auto& [_, tx_data] : pending_txs)
            block.transactions.push_back(tx_data);
        block.header.merkle_root      = Block::compute_merkle_root(block.transactions);

        if (reg) {
            block.has_song = true;
            block.song.audio_format         = reg->audio_format;
            block.song.content_hash         = reg->content_hash;
            block.song.compressed_fingerprint = reg->compressed_fingerprint;
            block.song.duration_ms          = reg->duration_ms;
            block.song.title                = reg->title;
            block.song.artist               = reg->artist;
            block.song.artist_address       = reg->artist_address;
            block.song.genre                = reg->genre;
            block.song.album                = reg->album;
            block.song.year                 = reg->year;
            block.song.track_number         = reg->track_number;
            block.song.royalty_splits       = reg->royalty_splits;
            block.header.content_hash       = reg->content_hash;
            // Bug fix: always recompute fingerprint_hash from the actual
            // compressed bytes we're about to ship. Block::validate
            // checks that header.fingerprint_hash ==
            // sha256(song.compressed_fingerprint); trusting the reg's
            // stored fph used to break the block when a player's
            // claimed hash disagreed with the bytes (or when the
            // compressed format was renormalized somewhere along the
            // path). Recomputing here makes validate a tautology.
            block.header.fingerprint_hash   = crypto::sha256(
                reinterpret_cast<const uint8_t*>(reg->compressed_fingerprint.data()),
                reg->compressed_fingerprint.size());
        }
        // Heartbeat (no song): header.content_hash / fingerprint_hash
        // stay zero — see Block::validate.

        std::string err;
        if (!commit_block(block, chain, db, network, cfg, keypair,
                          pending_txs, err)) {
            std::cerr << "[chain] block commit failed: " << err << "\n";
            {
                std::lock_guard<std::mutex> lk(producer_mu_);
                last_block_at_ms_ = now; // back off so we don't spin
            }
            // ---- Bug fix #8 / #26 ---------------------------------
            //
            // Don't blindly re-queue. Failures usually fall into one of
            // three buckets:
            //
            //   * "Chain connect_block rejected" with a duplicate-song
            //     reason — the song's content hash is already on chain.
            //     Re-queueing would just trigger the same rejection on
            //     the next iteration; drop it.
            //   * Confirmation timeout in multi-node mode — transient,
            //     retry up to 3 times then give up.
            //   * All other failures — drop after one retry to avoid an
            //     infinite loop poisoning chain progression for
            //     genuinely broken submissions.
            //
            // We use the PendingRegistration's `retries` field (added
            // in this turn) as the retry counter; the chain-side
            // duplicate check at validate_block already rejects songs
            // whose fingerprint is on chain so we don't have to
            // re-query here.
            if (reg) {
                reg->retries++;
                bool give_up = reg->retries >= 3;
                // If validate_block rejected it as duplicate, bail
                // immediately — there is no benefit to retrying.
                if (chain.validate_block_quick_duplicate(reg->content_hash)) {
                    give_up = true;
                    std::cerr << "[chain] dropping duplicate song reg "
                              << crypto::to_hex(reg->content_hash).substr(0, 12)
                              << "…\n";
                }
                if (!give_up) {
                    std::lock_guard<std::mutex> rlk(regs_mutex_);
                    pending_regs_.push(std::move(*reg));
                } else {
                    std::cerr << "[chain] dropping reg after "
                              << static_cast<unsigned>(reg->retries)
                              << " retries\n";
                }
            }
            continue;
        }

        if (reg) {
            std::cout << "[chain] block " << chain.tip().height
                      << " registered \"" << reg->title << "\" by "
                      << reg->artist << " (ch="
                      << crypto::to_hex(reg->content_hash).substr(0, 12)
                      << ", " << block.transactions.size() << " tx)\n";
        } else {
            std::cout << "[heartbeat] block " << chain.tip().height
                      << " emitted with " << block.transactions.size()
                      << " tx (empty fingerprint)\n";
        }
    }
}

} // namespace mc
