#include "candidate.h"
#include "../core/chain.h"
#include "../core/transaction.h"
#include "../storage/database.h"
#include "../network/manager.h"
#include "../audio/fingerprint.h"
#include "../audio/ogg_validator.h"
#include "../crypto/hash.h"
#include "../crypto/keys.h"
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
    // validator_enabled gates the heartbeat producer. Without this
    // gate every full node on the network minted its own heartbeat,
    // forking the chain at every block ("prev_hash break at height N"
    // floods on both directions). The flag was parsed from config but
    // never actually consulted, so all nodes acted as producers
    // regardless. Setting it false lets a node SERVE the chain
    // (answer songs.list, sync blocks, hold the swarm index) without
    // competing for block production — proper multi-producer
    // consensus (leader election + reorg) can layer on top later.
    if (!cfg.validator_enabled) {
        std::cout << "[chain] validator_enabled=false — running as "
                     "follower (sync + serve only, no block production)\n";
        return;
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

    // MODEL 1 — vote-free deterministic consensus.
    //
    // A block becomes canonical the instant chain.connect_block accepts
    // it. connect_block runs the full deterministic validation
    // (block.validate(): fingerprint/merkle commitment; prev_hash link
    // to the current tip; apply_transactions: every tx's signature,
    // nonce, and balance). There is no candidate registration, no self-
    // signed block confirmation, and no quorum wait — those were the
    // "vote" machinery, now removed. Every peer that later receives this
    // block re-derives the same verdict independently (BlockPropagator
    // INV/getdata + DHT, then ingest_block_bytes runs the identical
    // deterministic checks). Genesis is no longer special-cased: at
    // height 0 connect_block simply accepts the first valid block as the
    // chain's genesis.
    //
    // network / keypair are no longer used here (no candidate broadcast,
    // no block-level signing); consumed_txs is informational because the
    // mempool drain is folded into connect_block's leveldb batch.
    (void)network;
    (void)keypair;
    (void)consumed_txs;

    if (!chain.connect_block(block)) {
        err = "Chain connect_block rejected";
        return false;
    }

    // Gossip the new block out — INV broadcast to connected peers +
    // DHT-announce so multi-source catch-up can find this node.
    if (announcer_) announcer_(block.hash());

    // Best-effort .blk dump for the operator. Bucket by height/1000 so
    // no directory holds more than ~1000 files (NTFS degrades around 10M
    // files in a flat folder). Layout: blocks/00000123/00123456.blk
    try {
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
    } catch (...) {
        // Non-fatal — block is durable in LevelDB even if the .blk write fails.
    }

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
