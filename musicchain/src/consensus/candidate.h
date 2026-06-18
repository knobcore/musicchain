#pragma once
#include <cstdint>
#include "../core/block.h"
#include "../core/transaction.h"
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <condition_variable>

// Forward declarations
namespace mc        { class Chain; class Database; }
namespace mc::net   { class NetworkManager; struct NodeConfig; }
namespace mc::crypto{ struct KeyPair; }

namespace mc {

static constexpr uint32_t REQUIRED_CONFIRMATIONS  = 3;
static constexpr uint32_t BLOCK_TIMEOUT_SECONDS   = 300;

// If no song-bearing block lands within this window, the chain produces
// a heartbeat block carrying only the pending tx mempool — the empty-
// fingerprint case from the whitepaper §11.2. 5 min matches the user's
// spec.
static constexpr uint32_t HEARTBEAT_INTERVAL_MS   = 5 * 60 * 1000;

// PendingUpload + the upload pipeline were removed when the full node
// stopped ingesting audio bytes. Songs now reach the chain only through
// `PendingRegistration` (queued by fingerprint.submit).
//
// Player-submitted fingerprint registration: under the post-pivot
// architecture, songs enter the chain by a player fingerprinting a
// local file and submitting the digest + minimal metadata via
// fingerprint.submit. The full node never sees the audio bytes.
struct PendingRegistration {
    Hash256                   content_hash;
    Hash256                   fingerprint_hash;
    std::string               compressed_fingerprint;
    AudioFormat               audio_format = AudioFormat::OGG;
    uint32_t                  duration_ms = 0;
    std::string               title;
    std::string               artist;
    Address                   artist_address{};
    std::string               genre;
    std::string               album;
    uint16_t                  year         = 0;
    uint16_t                  track_number = 0;
    std::vector<RoyaltySplit> royalty_splits;
    std::string               announcing_peer_id; // who fingerprinted it
    // How many block-mint attempts this reg has burned. The producer
    // gives up after 3 so a single bad submission can't wedge the
    // chain forever (bug-fix #8).
    uint8_t                   retries          = 0;
};

struct BlockCandidate {
    Block                       block;
    std::vector<Confirmation>   received_confirmations;
    uint64_t                    created_at_ms;

    bool is_expired() const;
    bool is_final()   const { return received_confirmations.size() >= REQUIRED_CONFIRMATIONS; }
};

class CandidateManager {
public:
    CandidateManager()  = default;
    ~CandidateManager() { stop(); }

    // ---- Candidate tracking ----
    void add_candidate(const std::string& candidate_hash, BlockCandidate candidate);
    bool add_confirmation(const std::string& candidate_hash, const Confirmation& conf);
    std::optional<BlockCandidate> get_candidate(const std::string& hash) const;
    void cleanup_expired();
    std::vector<std::pair<std::string, BlockCandidate>> get_all() const;

    // ---- Block producer lifecycle ----
    void start(Chain& chain, Database& db,
               net::NetworkManager& network,
               const net::NodeConfig& cfg,
               const crypto::KeyPair& keypair);
    void stop();

    /// Player fingerprinted a song the chain doesn't know yet. Queue it
    /// for inclusion in the next block the heartbeat producer mints.
    /// Returns true if the registration was accepted (i.e. the
    /// content_hash isn't already known and isn't already queued).
    bool enqueue_registration(PendingRegistration reg);

    /// Number of pending registrations waiting to land in the next block.
    size_t pending_registration_count() const;

    /// External nudge: callers that have just dropped something into
    /// the mempool (e.g. action_bootstrap_founder after enqueueing the
    /// GRANT FOUNDER tx) call this so the heartbeat loop wakes
    /// immediately instead of waiting out the poll interval. No effect
    /// on the producer's internal state — pure notify.
    void wake();

    /// Fires after every successful chain.connect_block in
    /// commit_block. node_main wires this to
    /// BlockPropagator::announce_new_block so freshly-minted blocks
    /// gossip out as INV + get DHT-announced for multi-source catch-up.
    using BlockAnnouncer = std::function<void(const Hash256& hash)>;
    void set_block_announcer(BlockAnnouncer f) { announcer_ = std::move(f); }

private:
    BlockAnnouncer announcer_;

    mutable std::mutex                               mutex_;
    std::unordered_map<std::string, BlockCandidate>  candidates_;
    std::condition_variable                          confirm_cv_;

    // Heartbeat producer state. last_block_at_ms_ guarded by producer_mu_.
    mutable std::mutex                               producer_mu_;
    std::condition_variable                          heartbeat_cv_;
    std::thread                                      heartbeat_thread_;
    bool                                             running_ = false;
    uint64_t                                         last_block_at_ms_ = 0;
    // wake() sets this to true under producer_mu_ + notify_all(); the
    // wait_for predicate checks it so a notify accompanying a mempool
    // tx (rather than a pending_regs_ push) actually breaks out of the
    // sleep. Producer body resets it once the work is consumed.
    bool                                             wake_requested_ = false;

    // Pending player-submitted registrations. The heartbeat loop drains
    // these into freshly-minted blocks — one song record per block,
    // newest first.
    mutable std::mutex                               regs_mutex_;
    std::queue<PendingRegistration>                  pending_regs_;

    /// Finalize `block`: register as candidate, gather confirmations
    /// (self-sign in solo mode), connect to chain, write the .blk file,
    /// drain `consumed_txs` from the mempool. Returns true on success,
    /// populates `err` on failure.
    bool commit_block(Block& block,
                      Chain& chain, Database& db,
                      net::NetworkManager& network,
                      const net::NodeConfig& cfg,
                      const crypto::KeyPair& keypair,
                      const std::vector<std::pair<Hash256,
                                                  std::vector<uint8_t>>>& consumed_txs,
                      std::string& err);

    /// Heartbeat thread: every ~30 s, if no block has landed for
    /// HEARTBEAT_INTERVAL_MS, produce an empty-fingerprint block
    /// carrying the current mempool. Wakes immediately when a song
    /// registration is enqueued.
    void heartbeat_loop(Chain& chain, Database& db,
                        net::NetworkManager& network,
                        const net::NodeConfig& cfg,
                        const crypto::KeyPair& keypair);
};

} // namespace mc
