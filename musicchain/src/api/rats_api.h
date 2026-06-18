#pragma once
#include <cstdint>

#include "../core/chain.h"
#include "../consensus/candidate.h"
#include "../network/manager.h"
#include "../crypto/keys.h"
#include "../store/swarm.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" { typedef void* rats_client_t; }

namespace mc { class BlockPropagator; }

namespace mc::api {

class HttpServer; // forward — RatsApi borrows its verb handlers

/// `RatsApi` exposes the full node's verbs over librats typed messages.
/// Requests arrive on message type `musicchain.request` as JSON
/// `{req_id, type, body}`; replies go back on `musicchain.reply` as
/// `{req_id, status, body|error}`. The full node never serves or
/// ingests audio bytes under the post-pivot architecture — players
/// fingerprint locally and stream peer-to-peer. The verbs RatsApi
/// answers are chain queries (songs.list, songs.get, status), the swarm
/// announcement (fingerprint.submit), play sessions (session.start /
/// heartbeat / complete), and wallet queries.
///
/// RatsApi does not own the rats client; it borrows the one started by
/// `RatsLink` and the verb dispatch in `HttpServer`.
class RatsApi {
public:
    RatsApi(HttpServer& http,
            Chain& chain,
            CandidateManager& candidates,
            net::NetworkManager& network,
            Database& db,
            const net::NodeConfig& config,
            const mc::crypto::KeyPair& keypair);

    void start(rats_client_t client);
    void stop();

    /// Plug a BlockPropagator in. handle_request forwards every
    /// envelope whose `type` starts with `block.` to the propagator;
    /// on/off-peer callbacks are forwarded so the propagator can fire
    /// block.hello on connect and clean up state on drop.
    void set_block_propagator(BlockPropagator* bp) { propagator_ = bp; }

    /// Read-only handle to the in-memory swarm index so the full node
    /// TUI can render live song-count / member stats without going
    /// through an RPC round trip.
    store::SwarmIndex& swarm_index() { return swarm_; }

    /// Sign and publish a moderation action originating on this node.
    /// Applies the change to the local db + appends to the mod log so
    /// `iter_mod_log_since` will replay it, then broadcasts the signed
    /// envelope so every currently-connected peer (and, via re-broadcast,
    /// the rest of the mesh) converges. Returns false on a malformed
    /// action / value (e.g. bad hash) so the TUI can flash an error.
    bool publish_mod_action(const std::string&          action,
                             const std::string&          value,
                             const mc::crypto::KeyPair&  moderator_kp);

private:
    HttpServer&                 http_;
    Chain&                      chain_;
    CandidateManager&           candidates_;
    net::NetworkManager&        network_;
    Database&                   db_;
    net::NodeConfig             config_;
    mc::crypto::KeyPair         keypair_;
    rats_client_t               client_ = nullptr;
    BlockPropagator*            propagator_ = nullptr;

    static void on_request_cb(void* user_data, const char* peer_id,
                              const char* message_data);
    void handle_request(const std::string& peer_id, const std::string& body);
    void send_reply(const std::string& peer_id, const std::string& reply_json);

    /// Receive a moderation envelope from a peer (broadcast or sync push)
    /// — verify the signature against the moderator set, dedupe by sig,
    /// apply the hide / unhide to db, and persist to the mod log so
    /// downstream sync_since calls can replay it. No-op on duplicates,
    /// bad signatures, or unknown actions.
    static void on_mod_action_cb(void* user_data, const char* peer_id,
                                  const char* message_data);
    void handle_mod_envelope(const std::string& peer_id,
                              const std::string& payload_json,
                              bool broadcast_if_new);

    /// Push every mod-log entry strictly newer than `since_ts_ms` to the
    /// given peer as individual `musicchain.mod` messages. Called when a
    /// peer connects + requests `mod.sync_since`.
    void push_mod_log_since(const std::string& peer_id,
                             uint64_t            since_ts_ms);

    // Connection-state hooks for the efficient swarm protocol. The
    // full node treats "peer is rats-connected" as authoritative for
    // swarm availability — no per-track resubmits needed to keep songs
    // visible while the player is online.
    static void on_peer_connected_cb(void* user_data, const char* peer_id);
    static void on_peer_disconnected_cb(void* user_data, const char* peer_id);

    /// In-memory swarm of peer ids that have announced (via
    /// fingerprint.submit) that they hold the bytes for a given
    /// content_hash. stream.open returns this list so requesters can
    /// reach out to a swarm member directly.
    store::SwarmIndex swarm_;

    /// Periodically prunes ghost peers from [swarm_]. Started in
    /// [start], stopped in [stop].
    std::thread        prune_thread_;
    std::atomic<bool>  prune_running_{false};
};

} // namespace mc::api
