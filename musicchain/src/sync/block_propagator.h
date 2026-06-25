#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "../core/chain.h"
#include "../core/block.h"
#include "../network/manager.h"

extern "C" { typedef void* rats_client_t; }

namespace mc {

namespace net { class RatsLink; }

/// Hasher for `Hash256` (= `std::array<uint8_t, 32>`). The first 8 bytes
/// of a SHA-256 are already cryptographically uniform, so a memcpy into
/// size_t is a fine bucket key for unordered_map / unordered_set.
struct Hash256Hash {
    size_t operator()(const Hash256& h) const noexcept {
        size_t r = 0;
        std::memcpy(&r, h.data(), sizeof(r));
        return r;
    }
};

/// BlockPropagator — bitcoin-style block distribution over librats, with
/// BitTorrent-style DHT multi-source fetch on catch-up.
///
/// Replaces SyncManager + routes.get / relay.forward. Full nodes find
/// each other in two ways:
///
///   * librats DHT (the "A" path). Every full node DHT-announces a
///     well-known marker hash on start; DHT-find returns the set of
///     currently-online full nodes. No hardcoded VPS in the home
///     node's config required. `sync_seeds` in NodeConfig is still
///     honored as a fallback for fresh deploys whose DHT routing
///     table hasn't populated yet.
///   * librats DHT for individual block hashes (the "B" path). Each
///     full node DHT-announces every block hash it stores. On
///     catch-up the propagator can DHT-find any missing block hash
///     and fetch from multiple peers in parallel rather than serially
///     pulling everything from one peer.
///
/// Live new-block propagation uses INV gossip (push) — DHT-announce
/// takes seconds to propagate and is way too slow for blocks that
/// need sub-second confirmation. Bitcoin's INV → GETDATA → BLOCK is
/// transliterated 1:1 below.
///
/// Wire verbs (all envelopes are musicchain.request JSON):
///
///   block.hello       { tip_height, tip_hash, timestamp_ms }
///                     -- sent both directions right after librats
///                        peer connect. Reply body echoes the
///                        responder's tip so a hello pair takes one
///                        round trip.
///   block.getblocks   { locator: [hex_hash, ...] }
///                     -- bitcoin locator (tip, tip-1, tip-2, tip-4,
///                        tip-8, ..., genesis). Receiver walks back
///                        through locator until it finds a hash on
///                        its own chain (the fork point) then replies
///                        with up to kMaxInvCount hashes after it.
///                     reply: { hashes: [...] }
///   block.inv         { hashes: [...] }
///                     -- one-way push of newly-known block hashes.
///                        Receiver fires block.getdata for any hash
///                        it doesn't already have, then re-broadcasts
///                        the inv to its other peers (gossip).
///                     reply: { } (empty ack)
///   block.getdata     { hashes: [...] }
///                     -- request blocks by hash. Receiver responds
///                        with one block.data message per hash
///                        (chunked — never collapsed into a single
///                        giant reply). Hashes the receiver doesn't
///                        have come back in `notfound`.
///                     reply: { accepted: [...], notfound: [...] }
///   block.data        { hash, bytes_b64 }
///                     -- a single block, sent as a separate request
///                        (NOT a reply) so multiple blocks can stream
///                        back from different peers in parallel
///                        without one fat reply blocking the rest.
///                     reply: { } (empty ack)
///
/// Validation reuses the same five-step checks SyncManager ran:
/// deserialize, hash match, prev_hash, Block::validate, confirmation
/// quorum, hardcoded checkpoints, Chain::connect_block.
class BlockPropagator {
public:
    BlockPropagator(Chain& chain,
                    net::RatsLink& rats,
                    const net::NodeConfig& cfg);
    ~BlockPropagator();

    void start(rats_client_t client);
    void stop();

    /// Called by CandidateManager right after Chain::connect_block on a
    /// locally minted block. Two effects: (1) INV-broadcast to every
    /// connected librats peer that hasn't already told us they know
    /// the hash; (2) DHT-announce the block hash so peers doing
    /// multi-source catch-up can find this node. Idempotent on
    /// duplicates -- safe to call regardless of block origin.
    void announce_new_block(const Hash256& hash);

    /// Number of connected peers that have identified as full nodes via
    /// block.node_hello (role=="full-node"). This is the connectivity-gate
    /// input: announce_new_block HOLDS (buffers, no INV/DHT fan-out) while
    /// this is 0 so an isolated node doesn't shout a fork into the void, and
    /// flushes the moment it becomes >=1. Minting is never gated on it.
    /// Best-effort (counts live peers_ entries); never a safety invariant.
    size_t full_node_peer_count();

    /// Inbound dispatch from rats_api::handle_request for envelopes
    /// whose `type` begins with "block.". Returns the JSON body to
    /// embed in the musicchain.reply. Always returns a body; status
    /// is always "ok" -- block.* failures are signaled in the body
    /// itself (e.g. via `notfound`) so the wire-level protocol stays
    /// straightforward.
    nlohmann::json handle_request(const std::string& peer_id,
                                  const std::string& type,
                                  const nlohmann::json& body);

    /// Forwarded from rats_api's connect / disconnect hooks.
    void on_peer_connected(const std::string& peer_id);
    void on_peer_disconnected(const std::string& peer_id);

private:
    // -- peer state ---------------------------------------------------
    struct PeerState {
        uint32_t tip_height{0};
        uint64_t tip_weight{0};   // #8: peer's cumulative-play fork weight
        Hash256  tip_hash{};
        bool     hello_received{false};
        // node.hello: set true once this peer sends a block.node_hello with
        // role=="full-node". This — NOT hello_received (a player could answer
        // block.hello too) — is the discriminator that counts toward the
        // connectivity gate. node_id binds the transport peer_id to the peer's
        // chain identity (advisory until signature verification is enabled).
        bool        is_full_node{false};
        std::string node_id;
        std::chrono::steady_clock::time_point node_hello_at{};
        /// Hashes we know this peer already knows about — used to
        /// suppress re-INV (bitcoin's `setInventoryKnown`). Bounded
        /// via a periodic trim in dht_announce_loop so a long-lived
        /// peer connection doesn't grow it without limit.
        std::unordered_set<Hash256, Hash256Hash> known;
        /// Hashes we've sent block.getdata to this peer for and not
        /// yet received block.data on. Used to spread the next batch
        /// across other peers if one stalls.
        std::unordered_set<Hash256, Hash256Hash> in_flight;
        std::chrono::steady_clock::time_point   in_flight_since{};
    };

    // -- worker loops -------------------------------------------------
    void dht_announce_loop();   // re-announces stored block hashes + marker
    void apply_loop();          // drains pending_blocks_ into chain in order
    void stall_loop();          // re-issues stalled getdata, drives catch-up
    void seed_dial_loop();      // dials explicit sync_seeds when configured

    // -- helpers ------------------------------------------------------
    /// Bitcoin block locator: tip, tip-1, tip-2, tip-4, tip-8, ...,
    /// genesis. Lets the peer find our fork point in O(log n) hash
    /// compares without having to scan its entire chain.
    std::vector<Hash256> build_locator() const;

    /// Walk the locator newest-first; first hash present on our own
    /// chain is the fork point. Return up to kMaxInvCount hashes
    /// starting at fork_point + 1. Empty result means we have nothing
    /// the requester doesn't.
    std::vector<Hash256> hashes_after_locator(
        const std::vector<Hash256>& locator) const;

    /// Send a musicchain.request envelope. Fire-and-forget at the
    /// application level — block.* acks (empty bodies) are noise we
    /// don't need to await; stalls recover via stall_loop().
    void send_request(const std::string& peer_id,
                      const std::string& type,
                      const nlohmann::json& body);

    /// Five-step validation + connect_block path SyncManager used.
    /// Returns true on successful connect (which then triggers
    /// announce_new_block for gossip).
    bool ingest_block_bytes(const std::vector<uint8_t>& bytes,
                            const Hash256& expected_hash);

    /// Unconditional INV-broadcast + DHT-announce of one hash — the body of
    /// the old announce_new_block. Called only once we have a full-node peer
    /// (announce_new_block gates entry to this).
    void do_announce(const Hash256& hash);

    /// Drain pending_announce_ (blocks we minted while isolated) through
    /// do_announce. Called on announce when a peer exists, and on the 0->1
    /// full-node-peer transition in the node.hello handler.
    void flush_pending_announce();

    /// Queue a getdata for `hash` against any peer that doesn't
    /// currently have it in their `in_flight` set, preferring peers
    /// whose `known` set already contains the hash. Sends at most one
    /// getdata per hash per call. If no connected peer is a candidate,
    /// triggers a DHT search for the hash (bounded by kDhtSearchMinGap
    /// to avoid spamming).
    void schedule_getdata(const Hash256& hash);

    // -- DHT ----------------------------------------------------------
    /// sha1-hex of an arbitrary input. librats's DHT key space is 20
    /// bytes (BitTorrent BEP-5), so we sha1 anything we'd otherwise
    /// want to use as a key (block hashes, marker strings).
    static std::string sha1_hex(const void* data, size_t n);
    static std::string sha1_hex(const std::string& s) {
        return sha1_hex(s.data(), s.size());
    }
    /// DHT key for a single block — sha1 of the 32-byte block.hash.
    /// Two-step lookup: DHT search returns peer addresses; we then
    /// dial them and getdata for the actual sha256 hash, which catches
    /// the (astronomically unlikely) sha1 collision case.
    static std::string dht_key_for_block(const Hash256& h);

    /// Announce a single DHT key. librats's `rats_announce_for_hash`
    /// also kicks off a find for the same key as a side effect.
    void dht_announce(const std::string& key_hex);

    /// Fires from librats's DHT thread on a fresh batch of peer
    /// addresses. Per the musicchain librats patch's ownership
    /// contract, every entry + the array itself are heap-allocated
    /// and we MUST free them. We park each address as a pending dial
    /// (rats_connect → handshake → on_peer_connected) and let the
    /// normal block.hello / getblocks flow take over once the peer
    /// shows up in `peers_`.
    static void on_peers_found_cb(void* user_data,
                                  const char** peer_addresses,
                                  int count);

    // -- state --------------------------------------------------------
    Chain&                                     chain_;
    net::RatsLink&                             rats_;
    net::NodeConfig                            cfg_;
    rats_client_t                              client_{nullptr};
    std::string                                bootstrap_key_;

    std::mutex                                                            peers_mu_;
    std::unordered_map<std::string, PeerState>                            peers_;

    std::mutex                                                            pending_mu_;
    std::unordered_map<Hash256, Block, Hash256Hash>                       pending_blocks_;
    /// (connectivity gate) Hashes we minted while having zero full-node peers,
    /// held back from INV/DHT fan-out until a full-node peer appears, then
    /// flushed in order. Bounded by kPendingAnnounceCap — anything dropped is
    /// backfilled by the normal block.hello/getblocks reconcile on reconnect.
    /// Guarded by pending_mu_.
    std::deque<Hash256>                                                   pending_announce_;
    /// Hashes we want, in chain order. Drained from the front when
    /// pending_blocks_ supplies the next one.
    std::deque<Hash256>                                                   expected_sequence_;
    /// Last time we issued a DHT find for a hash (rate-limit to one
    /// search per kDhtSearchMinGap).
    std::unordered_map<Hash256,
                       std::chrono::steady_clock::time_point,
                       Hash256Hash>                                       dht_searched_at_;

    /// Hashes we've already DHT-announced this run, so dht_announce_loop
    /// doesn't waste cycles re-announcing every block on every sweep.
    /// Cleared when kReannounceMin elapses so DHT entries don't expire.
    std::unordered_set<Hash256, Hash256Hash>                              announced_;
    std::chrono::steady_clock::time_point                                 last_full_announce_{};

    /// (#11) Per-peer last block.hello time so a flapping peer reconnecting
    /// can't re-trigger block.hello → getblocks catch-up on every connect.
    /// Pruned of stale entries on access, so it stays bounded by active peers.
    /// Guarded by peers_mu_.
    std::unordered_map<std::string,
                       std::chrono::steady_clock::time_point>             last_hello_at_;

    std::thread             announce_thread_;
    std::thread             apply_thread_;
    std::thread             stall_thread_;
    std::thread             seed_thread_;
    std::condition_variable apply_cv_;
    std::atomic<bool>       running_{false};

    static constexpr uint32_t                kMaxInvCount         = 32;
    static constexpr uint32_t                kMaxGetdataInFlight  = 16;
    static constexpr std::chrono::seconds    kGetdataStall{20};
    static constexpr std::chrono::seconds    kDhtSearchMinGap{30};
    static constexpr std::chrono::minutes    kReannounceMin{30};
    /// (#11) cooldown between block.hello to the same peer across reconnects.
    static constexpr std::chrono::seconds    kHelloCooldown{15};
    /// (#8) cap on a peer's `known` set; cleared past this so a long-lived
    /// connection can't grow it without bound (a re-INV is harmless/deduped).
    static constexpr size_t                  kKnownCap            = 8192;
    /// (connectivity gate) cap on held-while-isolated announce hashes.
    static constexpr size_t                  kPendingAnnounceCap  = 4096;
    /// Above this height we DHT-announce every block on startup; below
    /// it we walk the chain in batches and announce in bursts (gives
    /// brand-new nodes a chance to find the chain quickly via DHT).
    static constexpr uint32_t                kStartupAnnounceBurst = 64;
};

} // namespace mc
