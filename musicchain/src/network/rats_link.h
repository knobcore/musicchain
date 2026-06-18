#pragma once

#include "../core/block.h"
#include "../net/load_monitor.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>

extern "C" { typedef void* rats_client_t; }

namespace mc::net {

// Hard-coded VPS used as the rendezvous server for every node.
constexpr const char* MC_VPS_HOST = "85.239.238.226";
// librats peer-to-peer talks plain TCP on a normal-range port (8080 is
// the librats project default; we follow it). HTTP/3 — for the API
// surface in `src/transport/h3_server.cpp` — runs on a separate UDP/443
// listener via msh3. STUN is handled by librats internally against the
// public stun.l.google.com server (see rats_link.cpp), so we no longer
// need MC_VPS_STUN_PORT.
constexpr uint16_t    MC_VPS_RATS_PORT = 8080;

/// Topic used for full-node route gossip. Full nodes publish their punched-
/// through endpoint to this topic every 15 minutes; the VPS mini-node
/// subscribes and aggregates the routing table.
constexpr const char* MC_ROUTES_TOPIC = "musicchain.routes";

// Typed librats message kinds for consensus. We use rats_broadcast_message /
// rats_on_message rather than GossipSub topics because the GossipSub layer
// has a several-second mesh form-up window we can't tolerate for block
// production — every full node accepts inbound consensus messages from any
// validated peer on the same direct channel routes.get uses.
//
// MC_MSG_BLOCK_CANDIDATE — producer → validators. Body: JSON
//   { "block_b64": "<base64 Block::serialize()>" }
// Receivers re-deserialize, run validate_block + duplicate-fingerprint
// check, and if both pass reply with a confirmation via the next msg type.
//
// MC_MSG_BLOCK_CONFIRMATION — validator → producer. Body: JSON
//   { "block_hash": "<hex>",
//     "validator_id": "<hex>",
//     "pubkey": "<hex compressed pubkey>",
//     "signature": "<hex 64-byte ECDSA>" }
// Producer routes into CandidateManager::add_confirmation.
constexpr const char* MC_MSG_BLOCK_CANDIDATE    = "mc:block_candidate";
constexpr const char* MC_MSG_BLOCK_CONFIRMATION = "mc:block_confirmation";

/// Wraps the librats C client so every musicchain-node can:
///   1. Punch through NAT via STUN at the VPS
///   2. Connect to the VPS bootstrap peer (and through it discover other nodes)
///   3. Accept inbound rats connections from phones (Flutter player)
///   4. Stream raw audio/block bytes peer-to-peer once the punch succeeds
///   5. Publish its routing record (node_id + public_address + api_port) to
///      the VPS mini-node every 15 minutes via the MC_ROUTES_TOPIC GossipSub
///      topic.
class RatsLink {
public:
    /// `node_id_hex`     – hex-encoded sha256(node_pubkey).
    /// `listen_port`     – librats UDP port.
    /// `own_api_port`    – HTTP API port (what other nodes / players will use).
    RatsLink(uint16_t listen_port,
             std::string node_id_hex,
             uint16_t    own_api_port);
    ~RatsLink();

    bool start();
    void stop();

    // Public address discovered via STUN at the VPS ("ip:port"), empty if unknown.
    std::string public_address() const;

    // 40-hex-char librats peer id (sha1-shaped). Empty if not yet started.
    std::string rats_peer_id() const;

    // Underlying librats client. Borrowed — do not destroy. Used by RatsApi to
    // register its own message handlers and send replies on the same client.
    rats_client_t client() const { return client_; }

    // Peers we are currently connected to (rats peer ids).
    std::vector<std::string> peer_ids() const;

    // Broadcast raw bytes to every connected rats peer.
    void broadcast(const void* data, size_t size);

    // ---- Consensus broadcast / receive ------------------------------
    //
    // Producer side: fan out a fresh candidate to every validated peer.
    // `block_bytes` is Block::serialize(); we base64-encode and wrap in
    // a tiny JSON envelope before handing to rats_broadcast_message.
    void publish_block_candidate(const std::vector<uint8_t>& block_bytes);

    // Validator side: respond to a candidate with this signed confirmation.
    void publish_confirmation(const std::string& block_hash_hex,
                              const ::mc::Confirmation& c);

    // Wire up the validator-side handler that runs whenever a peer
    // broadcasts a block candidate. Receives the raw serialized bytes;
    // caller deserializes + validates + sends back a confirmation.
    using BlockCandidateHandler =
        std::function<void(std::vector<uint8_t> /*block_bytes*/)>;
    void set_block_candidate_handler(BlockCandidateHandler h);

    // Wire up the producer-side handler that runs when a validator
    // sends back a confirmation. Routes into CandidateManager.
    using ConfirmationHandler =
        std::function<void(std::string /*block_hash_hex*/,
                            ::mc::Confirmation)>;
    void set_confirmation_handler(ConfirmationHandler h);

    // Publish our routing record to MC_ROUTES_TOPIC right now (also done
    // automatically every 15 minutes by the background thread).
    void publish_route_now();

    // Wire a LoadMonitor whose current snapshot is added to every
    // outgoing routes record. Players use the published load_score +
    // is_busy + net_bps to pick the lightest full node.
    void set_load_monitor(LoadMonitor* lm) { load_monitor_ = lm; }

private:
    uint16_t       listen_port_;
    std::string    node_id_hex_;
    uint16_t       own_api_port_;
    rats_client_t  client_ = nullptr;
    std::atomic<bool> running_{false};
    mutable std::mutex pub_mu_;
    std::string    public_addr_;
    std::string    rats_peer_id_;
    LoadMonitor*   load_monitor_ = nullptr;

    // List of bootstrap mini-nodes (VPSes) we dial at startup and re-dial
    // from the watchdog whenever validated_peer_count() == 0. Populated
    // from the env var MUSICCHAIN_VPS_BOOTSTRAP=host1:port1,host2:port2
    // and falls back to the legacy MC_VPS_HOST:MC_VPS_RATS_PORT when the
    // env var is unset. Multi-entry support lets a node sit on a mesh of
    // VPSes so any one of them going down still leaves the route alive.
    struct VpsBootstrap { std::string host; uint16_t port; };
    std::vector<VpsBootstrap> bootstrap_vps_;
    void load_bootstrap_list();

    std::thread       route_thread_;
    std::atomic<bool> route_thread_running_{false};
    void route_loop();
    void observe_public_address_via_vps();
    void redial_vps();
    std::string build_route_json() const;

    // Consensus handlers (default-empty until node_main wires them).
    mutable std::mutex      handlers_mu_;
    BlockCandidateHandler   on_block_candidate_;
    ConfirmationHandler     on_confirmation_;
    static void on_candidate_msg_trampoline(void* user_data,
                                            const char* peer_id,
                                            const char* message_data);
    static void on_confirmation_msg_trampoline(void* user_data,
                                               const char* peer_id,
                                               const char* message_data);

public:
    // Validated peer count — useful for consensus (knowing when the
    // multi-node confirmation path should run vs. the solo fast-path).
    int  validated_peer_count() const;
};

} // namespace mc::net
