#pragma once
#include <cstdint>
#include "../core/chain.h"
#include "../consensus/candidate.h"
#include "../network/manager.h"
#include "../crypto/keys.h"
#include <mutex>
#include <string>
#include <unordered_map>

namespace mc::api {

// Per-heartbeat sample: wall-clock time when the node received the
// heartbeat, plus the position_ms inside the song the player claimed it
// was at. Used at session.complete to decide whether the listener
// genuinely consumed enough audio to deserve a mint.
struct HeartbeatSample {
    uint64_t wall_ms;
    uint64_t position_ms;
};

// Active play session (in-memory)
struct PlaySession {
    Hash256  session_id;
    Hash256  content_hash;
    Hash256  block_hash;
    Address  player_address;
    // Per-stream reward lanes (PlayProof v2): the peer that SERVED the bytes and
    // the relay (mini-node) that carried the stream, reported by the player at
    // session.start. Zero when the player didn't report them (legacy / direct).
    Address  seeder_address{};
    Address  mini_node_address{};
    uint64_t start_timestamp;
    uint64_t last_heartbeat;
    uint32_t heartbeat_count;
    bool     completed = false;

    // Position samples in arrival order. The last entry is also reflected
    // in last_heartbeat for fast expiry checks. We don't bound this
    // explicitly because a session times out after TIMEOUT_MS of silence
    // and is dropped from the map anyway.
    std::vector<HeartbeatSample> samples;

    static constexpr uint64_t TIMEOUT_MS = 120000;
    bool is_expired(uint64_t now_ms) const {
        return !completed && (now_ms - last_heartbeat) > TIMEOUT_MS;
    }
};

/// `HttpServer` is the verb container — every API request (whether arriving
/// over librats / mc_rats_quic or the new HTTP/3 listener) dispatches to
/// one of the verb_* methods below. It does not own a socket of its own
/// anymore; the HTTP/3 listener lives in `transport/h3_server.{h,cpp}` and
/// calls these methods, and the rats RPC router (`api/rats_api.cpp`) does
/// the same on the QUIC peer channel.
class HttpServer {
public:
    HttpServer(Chain& chain, CandidateManager& candidates,
               net::NetworkManager& network, Database& db,
               const net::NodeConfig& config,
               const mc::crypto::KeyPair& keypair);
    ~HttpServer();

    bool start();
    void stop();

    // ---- Verb handlers -----------------------------------------------
    std::pair<int, std::string> verb_status()                                { return get_status(); }
    std::pair<int, std::string> verb_dht_peers()                             { return get_dht_peers(); }
    std::pair<int, std::string> verb_songs_list()                            { return get_songs_list(); }
    std::pair<int, std::string> verb_song_get(const std::string& hash)       { return get_song(hash); }
    std::pair<int, std::string> verb_wallet_balance(const std::string& addr) { return get_balance(addr); }
    std::pair<int, std::string> verb_wallet_escrow_balance(const std::string& addr)
        { return get_escrow_balance(addr); }
    std::pair<int, std::string> verb_wallet_nonce(const std::string& addr)   { return get_wallet_nonce(addr); }
    // Submit a SENDER-SIGNED TransferTx. Reuses post_transfer verbatim so the
    // rats path has IDENTICAL security to the HTTP route: verify_signature
    // (ECDSA + pubkey→from_address) + nonce replay check + balance, then mempool.
    // The node never moves funds without the sender's signature.
    std::pair<int, std::string> verb_wallet_transfer(const std::string& body)
        { return post_transfer(body); }
    std::pair<int, std::string> verb_session_start(const std::string& body)
        { return post_session_start(body); }
    std::pair<int, std::string> verb_session_heartbeat(const std::string& sid,
                                                       const std::string& body)
        { return post_session_heartbeat(sid, body); }
    std::pair<int, std::string> verb_session_complete(const std::string& sid,
                                                      const std::string& body)
        { return post_session_complete(sid, body); }
    std::pair<int, std::string> verb_songs_search_query(const std::string& q);
    std::pair<int, std::string> verb_songs_search_artist(const std::string& a);
    std::pair<int, std::string> verb_songs_search_genre(const std::string& g);

    // verb_song_audio + verb_upload_submit + verb_upload_status were
    // removed when the chain stopped ingesting and serving audio bytes.
    // Audio lives only with the players that announced themselves via
    // fingerprint.submit; clients hit them via the swarm list returned
    // from stream.open in rats_api.cpp.

private:
    Chain&                chain_;
    CandidateManager&     candidates_;
    net::NetworkManager&  network_;
    Database&             db_;
    net::NodeConfig       config_;
    mc::crypto::KeyPair   node_keypair_;

    mutable std::mutex                            sessions_mutex_;
    std::unordered_map<std::string, PlaySession>  sessions_;

    // Route handlers returning JSON response body + HTTP status code
    std::pair<int, std::string> get_status();
    std::pair<int, std::string> get_peers();
    std::pair<int, std::string> get_dht_peers();
    std::pair<int, std::string> get_block(const std::string& hash_hex);
    std::pair<int, std::string> get_block_at_height(uint32_t height);
    std::pair<int, std::string> get_songs_list();
    std::pair<int, std::string> get_song(const std::string& content_hash_hex);
    std::pair<int, std::string> get_balance(const std::string& address_hex);
    std::pair<int, std::string> get_escrow_balance(const std::string& address_hex);
    std::pair<int, std::string> post_session_start(const std::string& body);
    std::pair<int, std::string> post_session_heartbeat(const std::string& session_id,
                                                        const std::string& body);
    std::pair<int, std::string> post_session_complete(const std::string& session_id,
                                                       const std::string& body);
    std::pair<int, std::string> post_wallet_create();
    std::pair<int, std::string> get_wallet_address();
    std::pair<int, std::string> get_wallet_nonce(const std::string& address_hex);
    std::pair<int, std::string> post_moderator_release(const std::string& body);
    std::pair<int, std::string> delete_song(const std::string& content_hash_hex,
                                             const std::string& body);
    std::pair<int, std::string> post_transfer(const std::string& body);
    std::pair<int, std::string> post_net_announce(const std::string& body);
    std::pair<int, std::string> post_sync_block(const std::string& body);

    // Session helpers
    std::string generate_session_id() const;
    PlaySession* find_session(const std::string& session_id);

    std::pair<int, std::string> _do_songs_search(const std::string& artist,
                                                 const std::string& genre,
                                                 const std::string& q);
};

} // namespace mc::api
