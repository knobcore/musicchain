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
#include <condition_variable>
#include <deque>
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
    /// player is actually serving. wallet_hex is the lowercase 40-char hex
    /// of the 20-byte address.
    ///
    /// Signed-hello-only model: discovery trusts ONLY `last_verified_ms` — the
    /// server-clock time of the wallet's last VALID signed presence.hello. A
    /// wallet is discoverable while that is within kPresenceTtlMs. We do NOT
    /// gate on swarm is_online, because for a relayed player that signal is
    /// both flaky (a mini-node-forwarded swarm.peer_offline fires when a
    /// download saturates the relay -> the "download kills the library" bug)
    /// AND forgeable (peer_id is public, returned in stream.open replies, and
    /// swarm.peer_online is unauthenticated, so is_online can be spoofed). The
    /// player re-signs every ~30 s, so a live binding stays well inside the
    /// window; a departed one falls out of discovery after kPresenceTtlMs and
    /// an attacker cannot forge a signed hello to keep it alive. Replay of a
    /// captured hello is blocked by a timestamp skew check + per-wallet ts
    /// monotonicity (last_ts). A *direct* transport disconnect is authoritative
    /// and still erases the binding immediately.
    struct PresenceBinding {
        std::string peer_id;              // live player librats peer_id
        uint64_t    last_verified_ms = 0; // server-clock ms of last VALID signed hello
        uint64_t    last_ts          = 0; // payload ts of last accepted hello (replay guard)
    };
    static constexpr uint64_t kPresenceTtlMs       = 180'000; // discoverable window after a signed hello (~6 beats) — survives a download saturating the relay
    // ts is UTC epoch ms (timezone-independent): an NTP-synced device is within
    // ms of our clock anywhere on Earth. This 2-min window only forgives a
    // device whose wall clock is actually mis-set, and bounds hello replay.
    static constexpr uint64_t kPresenceHelloSkewMs = 120'000;
    // Keep a dead binding >= 2x the skew so per-wallet ts monotonicity blocks a
    // replayed hello right up until the skew check rejects it, even if a
    // player's clock ran up to one skew-window AHEAD of ours when it bound.
    static constexpr uint64_t kPresenceReapMs      = 300'000;
    std::mutex                                          wallet_presence_mu_;
    std::unordered_map<std::string, PresenceBinding>    wallet_to_peer_;        // wallet_hex(40) -> binding
    std::unordered_map<std::string, std::string>        peer_to_wallet_player_; // peer_id -> wallet_hex(40)

    static void on_request_cb(void* user_data, const char* peer_id,
                              const char* message_data);
    void handle_request(const std::string& peer_id, const std::string& body);
    void send_reply(const std::string& peer_id, const std::string& reply_json);

    /// DB2 discovery helper (defined in rats_api.cpp): returns the live
    /// player peer_ids serving a given canonical song (holders of the hash
    /// that currently have a presence binding + are online).
    std::vector<std::string>      online_peers_for_song_(const Hash256& ch);
    /// Set of currently-live wallet hexes (presence-bound + within
    /// kPresenceTtlMs), snapshotted under wallet_presence_mu_. Feeds the
    /// single-pass songs.list snapshot (LibraryStore::online_snapshot).
    std::unordered_set<std::string> live_wallets_();

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
    /// Caller MUST hold pd_mu_ (its only callers — handle_relay_report /
    /// handle_relay_receipt — already do) so the read-modify-(credit)-delete of
    /// a pd: row is atomic against the other leg's concurrent worker.
    void try_corroborate(const std::string& delivery_id_hex);
    /// TTL-reap pd: pending-delivery rows whose 3-way corroboration never
    /// completed (no matching relay.report + relay.receipt) within
    /// kDeliveryTtlMs. Called from the prune thread so stream.open's
    /// mint_delivery can't leak orphan rows. (#5)
    void reap_stale_deliveries_();
    static constexpr uint64_t kDeliveryTtlMs = 30ULL * 60ULL * 1000ULL; // 30 min
    // Serialises the get->parse->mutate->put (and corroborate->credit->del) of
    // a pending-delivery (pd:) row. Necessary now that handle_request runs on
    // the RPC worker pool (#1): relay.report and relay.receipt for the SAME
    // delivery_id can otherwise be processed by two workers at once, losing one
    // leg's flag bit (credit never fires) or — worse — letting both pass the
    // `(flags & 7)==7` check and double-credit the mini. Was implicitly safe
    // when every RPC ran on the single librats io thread. LEAF mutex: held only
    // around leveldb row ops + relay_tracker_->increment (its own lock); never
    // across a librats send, never nested under peers_mutex_/io_mutex_/tx_mutex.
    std::mutex pd_mu_;

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

    // ---- RPC worker pool (off the librats io thread) ------------------
    //
    // librats runs accept + recv + decrypt + every on_message callback on a
    // SINGLE io thread, and that same thread flushes relay-forward / outbound
    // sends. Running handle_request (fuzzy-fingerprint scan, ECIES, leveldb,
    // play_proof replay) inline on it starves the flush -> relay-expire +
    // stream.open timeouts. on_request_cb instead enqueues (peer_id, message)
    // here and returns immediately; a small bounded pool of workers runs
    // handle_request (which calls send_reply -> rats_send_message itself).
    //
    // Thread-safety: the work queue has its OWN mutex (rpc_mu_), independent of
    // and never nested inside librats' peers_mutex_/io_mutex_ or the per-cipher
    // tx_mutex — a worker takes rpc_mu_ only to pop, then RELEASES it before
    // touching anything else, so it never holds rpc_mu_ across a librats send.
    // handle_request's own shared state (swarm_, library_, wallet_presence_mu_,
    // peer_to_wallet_mu_, leveldb) is already internally synchronised and was
    // already reachable concurrently (broadcast handlers run on the io thread
    // too); moving it to N workers only widens that existing concurrency, it
    // introduces no new lock-order edge. Replies may now interleave across
    // verbs, but every reply carries its req_id so the player already matches
    // responses to requests out of order — no per-peer ordering is required.
    // A job is either an inbound RPC envelope (kRpc: run handle_request) or a
    // freshly-connected peer's anti-entropy handshake (kConnect: run
    // do_peer_connect_handshake_). Both are off-io-thread work that share the
    // one bounded pool, so a connect storm and an RPC flood draw from the same
    // budget instead of each spawning unbounded io-thread work.
    enum class RpcKind { kRpc, kConnect };
    struct RpcJob { RpcKind kind; std::string peer_id; std::string message; };
    std::mutex                       rpc_mu_;
    std::condition_variable          rpc_cv_;
    std::deque<RpcJob>               rpc_queue_;
    std::vector<std::thread>         rpc_workers_;
    bool                             rpc_running_ = false;   // guarded by rpc_mu_
    // Backpressure: if a flood outpaces the workers we drop the OLDEST queued
    // job (a stale request whose caller has likely already timed out) rather
    // than grow unbounded. Sized for the single-VPS relay fan-in.
    static constexpr size_t          kRpcQueueMax = 1024;
    void rpc_worker_loop_();
    void enqueue_rpc_(std::string peer_id, std::string message);
    // Returns true iff the kConnect job was actually queued; false if it was
    // refused (worker pool stopped) or dropped at insertion by the drop-oldest
    // backpressure. The caller (on_peer_connected_cb) uses this to stamp the
    // debounce timestamp ONLY on a real enqueue (#3), so a dropped handshake
    // doesn't suppress the retry for the full kConnectDebounceMs window.
    bool enqueue_connect_handshake_(std::string peer_id);

    // ---- on-connect anti-entropy cache + per-device debounce ----------
    //
    // on_peer_connected_cb used to walk the WHOLE library + playlist store and
    // fire 3 sends on EVERY connect — a reconnect storm amplifier. We instead:
    //  (a) cache the db2.sync {key -> version} summary as a pre-dumped JSON
    //      string, rebuilt lazily only after an actual library/playlist change
    //      (db2_summary_dirty_ set by the ingest paths); and
    //  (b) debounce per device: a peer reconnecting within kConnectDebounceMs
    //      skips the full handshake (its prior summary push is still current
    //      unless something changed, in which case the flood already reached it).
    // The connect work itself is dispatched onto the rpc worker pool so the io
    // thread's connection callback stays non-blocking.
    std::mutex                       db2_summary_mu_;
    std::string                      db2_sync_body_cache_;          // cached {lib,pl} object json
    bool                             db2_summary_dirty_ = true;     // rebuild on next use
    void mark_db2_summary_dirty_();                                 // ingest paths call this
    std::string db2_sync_body_();                                   // cached, rebuilds if dirty

    std::mutex                                   connect_debounce_mu_;
    std::unordered_map<std::string, uint64_t>    last_connect_handshake_ms_;
    static constexpr uint64_t kConnectDebounceMs = 15'000;
    void do_peer_connect_handshake_(const std::string& peer_id);   // runs on a worker

    // ---- diagnostics gate --------------------------------------------
    //
    // The per-RPC std::cout traces (recv type=…, stream.open …, fuzzy probe …)
    // are useful while a relay path is being stabilised but are pure overhead
    // (and lock contention on std::cout) on a busy node. Gate them behind
    // MC_RATS_DEBUG=1 in the environment, read once at start().
    std::atomic<bool>  debug_log_{false};
};

} // namespace mc::api
