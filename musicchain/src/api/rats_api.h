#pragma once
#include <cstdint>

#include "../core/chain.h"
#include "../consensus/candidate.h"
#include "../network/manager.h"
#include "../crypto/keys.h"
#include "../store/swarm.h"
#include "../store/library_store.h"   // DB2: wallet-keyed Roaring library store
#include <nlohmann/json_fwd.hpp>   // nlohmann::json fwd-decl for #10 handlers

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" { typedef void* rats_client_t; }

namespace mc { class BlockPropagator; }
namespace mc::net { class RelayCreditTracker; }
namespace mc::moderation { struct Envelope; }   // #4 forgery_report handler

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

    /// Plug a RelayCreditTracker in. The stream.open handler (the one
    /// binary-traffic verb the post-pivot full node exposes — chat /
    /// mod / routes are control-plane and don't qualify per the
    /// tracker's scope docstring) increments the count for the
    /// mini-node that relayed the request whenever it can resolve the
    /// mini-node's wallet via peer_to_wallet_. Mini-nodes self-identify
    /// over `mini.hello`, carrying a `wallet` field set by the
    /// musicchain mini-node patch.
    void set_relay_tracker(net::RelayCreditTracker* rt) { relay_tracker_ = rt; }

    /// Read-only handle to the in-memory swarm index so the full node
    /// TUI can render live song-count / member stats without going
    /// through an RPC round trip.
    store::SwarmIndex& swarm_index() { return swarm_; }

    /// Read-only handle to the wallet-keyed library store (DB2) so the TUI /
    /// future gossip layer can query libraries + holders without an RPC hop.
    store::LibraryStore& library_store() { return library_; }

    /// Synchronous verb dispatch for transports that don't go through
    /// the rats client. `body` is the same `{req_id, type, body}`
    /// envelope librats peers send. The reply is routed through the
    /// thread-local `g_ws_reply_sink` set by the caller. Originally
    /// added for the now-removed WebSocket bridge to the web player;
    /// retained as a generic hook in case future non-librats sources
    /// of RPC envelopes show up.
    void dispatch_for_bridge(const std::string& peer_id,
                              const std::string& body) {
        handle_request(peer_id, body);
    }

    /// Sign and publish a moderation action originating on this node.
    /// Applies the change to the local db + appends to the mod log so
    /// `iter_mod_log_since` will replay it, then broadcasts the signed
    /// envelope so every currently-connected peer (and, via re-broadcast,
    /// the rest of the mesh) converges. Returns false on a malformed
    /// action / value (e.g. bad hash) so the TUI can flash an error.
    bool publish_mod_action(const std::string&          action,
                             const std::string&          value,
                             const mc::crypto::KeyPair&  moderator_kp);

    /// Sign + gossip a forgery report (#4): the DeepAuditor calls this when
    /// a sampled block's audio fails to match its declared fingerprint, so
    /// the whole mesh can independently corroborate (K reports) / re-audit
    /// and drop the forged song. node_main wires it to DeepAuditor.
    void publish_forgery_report(const Hash256& content_hash,
                                const Hash256& block_hash,
                                float sim, const Hash256& audio_sha);

    /// Audio content-addressed store directory, so the forgery re-audit
    /// path can locate local bytes. Set once at startup by node_main.
    void set_audio_dir(const std::string& dir) { audio_dir_ = dir; }

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
    net::RelayCreditTracker*    relay_tracker_ = nullptr;
    std::string                 audio_dir_;   // for forgery re-audit (#4)

    /// Mini-node librats peer_id → mini-node wallet (20-byte raw EVM
    /// address). Populated when a peer pushes `mini.hello` with a
    /// `wallet` field; consulted by the stream.open handler to credit
    /// the right wallet for a relayed payload delivery. Empty entries
    /// (peer connected but never sent mini.hello, or self-identified
    /// without a wallet — e.g. another full node or a player) skip the
    /// credit silently so we never mint rewards into a placeholder
    /// address.
    std::mutex                                  peer_to_wallet_mu_;
    std::unordered_map<std::string, Address>    peer_to_wallet_;

    /// Wallet-presence binding (DB2 discovery): a player proves over
    /// `presence.hello` that it controls a wallet AND declares its live
    /// librats peer_id. We map wallet ⇄ peer_id so the DB2 LibraryStore
    /// (wallet-keyed) can be filtered down to songs a currently-online
    /// player is actually serving. Evicted when the player disconnects
    /// (direct disconnect callback or mini-node-forwarded
    /// swarm.peer_offline). wallet_hex is the lowercase 40-char hex of
    /// the 20-byte address.
    std::mutex                                       wallet_presence_mu_;
    std::unordered_map<std::string, std::string>     wallet_to_peer_;        // wallet_hex(40) -> live player peer_id
    std::unordered_map<std::string, std::string>     peer_to_wallet_player_; // peer_id -> wallet_hex(40)

    static void on_request_cb(void* user_data, const char* peer_id,
                              const char* message_data);
    void handle_request(const std::string& peer_id, const std::string& body);
    void send_reply(const std::string& peer_id, const std::string& reply_json);

    /// DB2 discovery helpers (defined in rats_api.cpp). The first returns
    /// the live player peer_ids serving a given canonical song (holders
    /// of the hash that currently have a presence binding + are online);
    /// the second returns the set of canonical-hash hex strings that any
    /// currently-online wallet's library contains.
    std::vector<std::string>      online_peers_for_song_(const Hash256& ch);
    std::unordered_set<std::string> online_library_hashes_();

    /// DB2 gossip — receive a wallet-signed `library.delta` over the
    /// MC_LIBRARY_TYPE broadcast channel: verify the signature against the
    /// claimed wallet, apply_delta() (version-gated/idempotent), and on a
    /// genuinely-new application re-broadcast so it floods the mesh. The
    /// version gate is what stops re-broadcast loops + converges every node.
    static void on_library_cb(void* user_data, const char* peer_id,
                              const char* message_data);
    /// Verify + apply a signed library-delta payload. Returns true iff it was
    /// newly applied (newer version). When `broadcast_if_new`, a newly-applied
    /// delta is re-broadcast to every peer (the flood). Shared by the
    /// library.delta request verb and the broadcast handler.
    bool ingest_library_delta(const std::string& payload_json,
                              bool broadcast_if_new);

    /// DB2 playlists — same wallet-signed, version-gated, flood-replicated
    /// path as library deltas, but the record is a wallet's ordered playlist
    /// (keyed by a 16-byte playlist id) over the MC_PLAYLIST_TYPE channel.
    static void on_playlist_cb(void* user_data, const char* peer_id,
                               const char* message_data);
    bool ingest_playlist(const std::string& payload_json,
                         bool broadcast_if_new);

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

    /// Receive a forgery report (#4) — node-signed (not moderator-gated).
    /// Verifies the signature, validates the named block carries the named
    /// content hash, records the report into the fr: tally, and marks the
    /// song deleted IFF we re-audit it as forged locally OR K independent
    /// reporters agree. Appends to the mod-log so it replays to late joiners.
    void handle_forgery_report(const std::string& peer_id,
                               const std::string& payload_json,
                               const moderation::Envelope& env,
                               bool broadcast_if_new);

    // #10 relay-reward triangulation (broker side). mint_delivery records a
    // pd:<id> pending-delivery row at stream.open; the two handlers verify
    // the mini's signed byte-report and the player's signed receipt against
    // that row; try_corroborate credits per min(relayed,received) byte once
    // all three legs agree, then retires the row (single-use ⇒ replay-proof).
    std::string mint_delivery(const Hash256& content_hash);
    bool handle_relay_report(const nlohmann::json& body);
    bool handle_relay_receipt(const nlohmann::json& body);
    void try_corroborate(const std::string& delivery_id_hex);

    // #5 "accept + record": persist the latest device attestation level per
    // device (`dal:<device_id>` → "<level>|<ts_ms>") so a future verifier
    // policy can tighten by level WITHOUT a client change. Never gates.
    void record_attestation_level(const std::string& device_id,
                                  const std::string& level);

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

    /// DB2 — wallet → Roaring(song ids) library store + song → wallets reverse
    /// index. Separate keyspace from the chain, outside consensus; attached to
    /// the same leveldb in start(). Edits replicate via signed deltas
    /// (apply_delta) over a gossip layer that lands in a follow-up.
    store::LibraryStore library_;

    /// Periodically prunes ghost peers from [swarm_]. Started in
    /// [start], stopped in [stop].
    std::thread        prune_thread_;
    std::atomic<bool>  prune_running_{false};
};

} // namespace mc::api
