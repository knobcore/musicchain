#include "rats_api.h"
#include "server.h"
#include "device_attestation.h"   // DeviceAttestationVerifier (#5)
#include "../audio/fingerprint.h"
#include "../moderation/mod_action.h"
#include "../util/traffic.h"
#include "../sync/block_propagator.h"
#include "../sync/deep_audit.h"   // audit_content + AuditResult for forgery re-audit (#4)
#include "../net/relay_credit_tracker.h"
#include "../core/transaction.h"

// mc_rats_quic.h was the old stub. We now link the real librats and
// consume its C bindings header so functions like rats_get_peer_info_json
// (used by the stun.observe verb below) are visible.
#include "librats_c.h"
#include "../crypto/hash.h"
#include "../crypto/signature.h"
#include "../crypto/ecies.h"
// h3_server include removed: the h3.request verb that wrapped HTTP/3-
// shaped tunneled calls is gone now that the player drives every verb
// through the native rats RPC. Restore behind MC_WITH_H3 when bringing
// HTTP/3 back.

#include <nlohmann/json.hpp>
#include <openssl/sha.h>
#include <openssl/rand.h>   // RAND_bytes for delivery_id (#10)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>

using json = nlohmann::json;

namespace mc::api {

constexpr const char* MC_REQUEST_TYPE = "musicchain.request";
constexpr const char* MC_REPLY_TYPE   = "musicchain.reply";
constexpr const char* MC_MOD_TYPE     = "musicchain.mod";
constexpr const char* MC_LIBRARY_TYPE = "musicchain.library";  // DB2 delta gossip
constexpr const char* MC_PLAYLIST_TYPE = "musicchain.playlist"; // DB2 playlist gossip

RatsApi::RatsApi(HttpServer& http,
                 Chain& chain, CandidateManager& candidates,
                 net::NetworkManager& network, Database& db,
                 const net::NodeConfig& config,
                 const mc::crypto::KeyPair& keypair)
    : http_(http), chain_(chain), candidates_(candidates), network_(network),
      db_(db), config_(config), keypair_(keypair) {
    // Hook the swarm map up to leveldb so announces survive full-node
    // restarts. Without this, every restart wiped the swarm and players
    // couldn't find each other again until they re-fingerprint.submit.
    swarm_.attach(db_);
    // DB2 — wallet-keyed library store. Slurps persisted libraries + rebuilds
    // the reverse (song → wallets) index from the same leveldb.
    library_.attach(db_);
}

void RatsApi::start(rats_client_t client) {
    if (client_) return;
    client_ = client;
    rats_on_message(client_, MC_REQUEST_TYPE,
                    &RatsApi::on_request_cb, this);
    rats_on_message(client_, MC_MOD_TYPE,
                    &RatsApi::on_mod_action_cb, this);
    // DB2 — wallet-signed library deltas flood over their own broadcast type.
    rats_on_message(client_, MC_LIBRARY_TYPE,
                    &RatsApi::on_library_cb, this);
    rats_on_message(client_, MC_PLAYLIST_TYPE,
                    &RatsApi::on_playlist_cb, this);
    // Connection-state authoritative swarm: track who's online so
    // entries from offline peers vanish from members() without needing
    // the per-track TTL renewal that the old per-file fingerprint.submit
    // floods used to drive.
    rats_set_connection_callback(client_,
                                 &RatsApi::on_peer_connected_cb, this);
    rats_set_disconnect_callback(client_,
                                 &RatsApi::on_peer_disconnected_cb, this);
    std::cout << "[rats-api] listening for " << MC_REQUEST_TYPE
              << " over librats (chain + swarm verbs only)\n";

    // Periodic prune so swarm_size on songs.list reflects actually-
    // reachable peers and not zombies from previous app installs. Ticks
    // every minute; SwarmIndex itself only drops entries older than
    // kStaleAfterMs (20 min).
    prune_running_ = true;
    prune_thread_ = std::thread([this] {
        while (prune_running_) {
            for (int i = 0; i < 60 && prune_running_; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (!prune_running_) return;
            swarm_.prune_stale();
        }
    });
}

void RatsApi::stop() {
    prune_running_ = false;
    if (prune_thread_.joinable()) prune_thread_.join();
    client_ = nullptr;
}

void RatsApi::on_request_cb(void* user_data, const char* peer_id,
                            const char* message_data) {
    auto* self = static_cast<RatsApi*>(user_data);
    if (!self || !peer_id || !message_data) {
        if (peer_id) rats_string_free(peer_id);
        if (message_data) rats_string_free(message_data);
        return;
    }
    // librats_c.cpp strdup's both args; we free explicitly after handling.
    self->handle_request(peer_id, message_data);
    rats_string_free(peer_id);
    rats_string_free(message_data);
}

void RatsApi::on_peer_connected_cb(void* user_data, const char* peer_id) {
    auto* self = static_cast<RatsApi*>(user_data);
    if (self && peer_id && *peer_id) {
        self->swarm_.mark_peer_online(peer_id);

        // Ask the freshly-connected peer to push us any moderation
        // envelopes newer than our latest known timestamp. Player peers
        // won't have any (they don't register the handler) and will
        // reply unknown_type / no-op; other full nodes will replay the
        // diff so this node converges to the same hide state.
        if (self->client_) {
            const uint64_t since = self->db_.latest_mod_log_ts();
            nlohmann::json req = {
                {"req_id", std::string("mod-sync-")
                              + std::to_string(moderation::now_ms())},
                {"type",   "mod.sync_since"},
                {"body",   {{"since_ts_ms", since}}},
            };
            rats_send_message(self->client_, peer_id,
                              MC_REQUEST_TYPE, req.dump().c_str());

            // DB2 anti-entropy: send the peer our library/playlist summary
            // ({key -> version}) so it can push back every record we're behind
            // on. Both sides do this on connect, so they converge. Players /
            // mini-nodes don't handle db2.sync and just no-op.
            {
                nlohmann::json lib = nlohmann::json::object();
                self->library_.for_each_library_payload(
                    [&](const Address& w, uint64_t ver, const std::string&) {
                        lib[crypto::to_hex(w.data(), w.size())] = ver;
                    });
                nlohmann::json pl = nlohmann::json::object();
                self->library_.for_each_playlist_payload(
                    [&](const Address& w, const std::array<uint8_t, 16>& id,
                        uint64_t ver, const std::string&) {
                        pl[crypto::to_hex(w.data(), w.size()) +
                           crypto::to_hex(id.data(), id.size())] = ver;
                    });
                nlohmann::json sreq = {
                    {"req_id", std::string("db2-sync-") +
                                  std::to_string(moderation::now_ms())},
                    {"type",   "db2.sync"},
                    {"body",   {{"lib", lib}, {"pl", pl}}},
                };
                rats_send_message(self->client_, peer_id,
                                  MC_REQUEST_TYPE, sreq.dump().c_str());
            }
        }

        // Block-distribution handshake: send block.hello so the new
        // peer learns our tip. If they're a full node and we're behind,
        // they'll respond with their tip + we'll fire block.getblocks
        // through the existing handle_request path.
        if (self->propagator_) {
            self->propagator_->on_peer_connected(peer_id);
        }
    }
    if (peer_id) rats_string_free(peer_id);
}

void RatsApi::on_peer_disconnected_cb(void* user_data, const char* peer_id) {
    auto* self = static_cast<RatsApi*>(user_data);
    if (self && peer_id && *peer_id) {
        // Hard evict instead of mark_peer_offline so dead-peer entries
        // don't linger up to 20 minutes (kStaleAfterMs) in stream.open
        // results when the transport says the peer is gone. The peer
        // can re-announce on reconnect via swarm.hello — cheap, since
        // the player ships a digest-based delta path.
        self->swarm_.evict_peer(peer_id);
        if (self->propagator_) self->propagator_->on_peer_disconnected(peer_id);
        // Drop the mini-node wallet cache entry so a re-connect under a
        // fresh peer_id can re-identify and we don't keep a stale entry
        // pinned forever.
        {
            std::lock_guard<std::mutex> lk(self->peer_to_wallet_mu_);
            self->peer_to_wallet_.erase(peer_id);
        }
    }
    // NOTE: do NOT rats_string_free here. The disconnect callback wrapper
    // in deps/librats/src/librats_c.cpp does NOT strdup peer_id (unlike
    // the connection callback, which does); the pointer we receive is a
    // borrowed view into a temporary std::string. Freeing it would be a
    // double-free → STATUS_HEAP_CORRUPTION (0xc0000374). The string is
    // already copied into swarm_.online_peers_ above, so we don't need
    // ownership anyway.
}

namespace {

// 20-byte SHA-1 over arbitrary bytes, hex-encoded — BEP-5 DHT key from a
// 32-byte canonical content_hash. The player derives the same key (via
// SwarmRegistry.dhtKeyFor) to announce/find content holders. Retained
// [[maybe_unused]] now that the full node no longer self-announces (DHT
// un-nerf P1), in case the tracker role is revived behind a real handler.
[[maybe_unused]] static std::string dht_key_for_content_hash(
    const Hash256& content_hash) {
    unsigned char out[SHA_DIGEST_LENGTH];
    SHA1(content_hash.data(), content_hash.size(), out);
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (int i = 0; i < SHA_DIGEST_LENGTH; ++i) {
        os << std::setw(2) << static_cast<int>(out[i]);
    }
    return os.str();
}

// Parse a verb-handler `std::pair<int, std::string>` (HTTP status + JSON body
// text) into the rats reply envelope. Status 200-299 is "ok"; everything else
// is treated as an error and the body becomes the error message.
json wrap_handler_result(const std::string& req_id,
                          const std::pair<int, std::string>& result) {
    json reply = {{"req_id", req_id}};
    const int    code = result.first;
    const auto&  text = result.second;
    if (code >= 200 && code < 300) {
        reply["status"] = "ok";
        try {
            // Most handlers return a JSON document — embed it directly.
            reply["body"] = text.empty() ? json::object() : json::parse(text);
        } catch (...) {
            reply["body"] = text; // fall back to opaque string
        }
    } else {
        reply["status"] = "http_" + std::to_string(code);
        reply["error"]  = text;
    }
    return reply;
}

json status_body(Chain& chain, net::NetworkManager& network,
                 const net::NodeConfig& config, size_t full_node_peers) {
    return {
        {"role",         "full-node"},
        {"node_id",      mc::crypto::to_hex(config.node_id)},
        {"chain_height", chain.tip().height},
        {"peer_count",   network.peer_count()},
        // (connectivity gate) full nodes we've handshaked via block.node_hello.
        // 0 here means the propagator is HOLDING new-block fan-out (isolated).
        {"connected_full_nodes", full_node_peers},
        {"own_ipv6",     network.own_ipv6_str()},
        {"api_port",     config.api_port},
        {"p2p_port",     config.p2p_port},
    };
}

} // namespace

// DB2 hex helper — declared here so it precedes its first use in
// handle_request's library/playlist verbs (and the ingest methods below).
namespace {
bool from_hex_fixed(const std::string& hex_in, uint8_t* out, size_t n) {
    // Tolerate an optional 0x / 0X prefix so either hex form parses.
    const std::string hex =
        (hex_in.size() >= 2 && hex_in[0] == '0' &&
         (hex_in[1] == 'x' || hex_in[1] == 'X')) ? hex_in.substr(2) : hex_in;
    if (hex.size() != n * 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; ++i) {
        const int hi = nib(hex[2 * i]), lo = nib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}
} // namespace

void RatsApi::handle_request(const std::string& peer_id,
                             const std::string& body) {
    json env;
    try {
        env = json::parse(body);
    } catch (...) {
        std::cerr << "[rats-api] envelope parse failed\n";
        return;
    }

    const std::string req_id = env.value("req_id", "");
    const std::string type   = env.value("type",   "");
    const json        in     = env.value("body",   json::object());

    // Bump the global per-verb counter so the TUI's F2 page can show
    // request volume by type. Cheap (mutex + atomic), runs once per RPC.
    if (!type.empty()) mc::util::traffic_counters().bump(type);

    if (req_id.empty() || type.empty()) {
        std::cerr << "[rats-api] missing req_id/type\n";
        return;
    }

    // Diagnostic: print verb + originator hint so we can tell whether the
    // VPS is actually attaching `originator_peer_id` on relayed envelopes.
    // Remove once the streaming relay path is stable.
    std::cout << "[rats-api] recv type=" << type
              << " from " << peer_id.substr(0, 12)
              << " originator=\""
              << env.value("originator_peer_id", std::string()) << "\"\n";

    // Any traffic from this peer is proof of life — refresh last_seen
    // on every SwarmMember for this peer_id so the prune cycle doesn't
    // drop an actively-talking peer.
    {
        const std::string origin = env.value("originator_peer_id",
                                             std::string());
        const std::string alive_pid = !origin.empty() ? origin : peer_id;
        if (!alive_pid.empty()) swarm_.touch_peer(alive_pid);
    }

    json reply;

    try {
        // ---- read-only verbs --------------------------------------------
        if (type == "status") {
            reply = {{"req_id", req_id}, {"status", "ok"},
                     {"body",   status_body(
                         chain_, network_, config_,
                         propagator_ ? propagator_->full_node_peer_count() : 0)}};
        } else if (type == "stun.observe") {
            // Echo back the address we see the caller from. Vanilla
            // librats stores the connection's `ip` and `port` in peer
            // info — no source_port distinction needed (the QUIC bridge
            // that used to overwrite `port` with the handshake's claimed
            // listen_port is gone). librats also exposes a native STUN
            // client; this verb is kept for clients that still call it
            // through the mini-node as a one-stop "what's my IP" hop.
            std::string addr;
            char* info = rats_get_peer_info_json(client_, peer_id.c_str());
            if (info) {
                try {
                    auto j = nlohmann::json::parse(info);
                    const std::string ip = j.value("ip", std::string());
                    const int port = j.value("port", 0);
                    if (!ip.empty() && port > 0) addr = ip + ":" + std::to_string(port);
                } catch (const nlohmann::json::exception&) {}
                rats_string_free(info);
            }
            reply = {{"req_id", req_id}, {"status", "ok"},
                     {"body", { {"observed_address", addr} }}};
        } else if (type == "dht.peers") {
            reply = wrap_handler_result(req_id, http_.verb_dht_peers());
        }
        // ---- bitcoin-style block distribution -----------------------
        //
        // Replaces the old chain.tip / chain.list_block_hashes /
        // chain.get_block trio. BlockPropagator owns every verb whose
        // type starts with "block." (hello, getblocks, inv, getdata,
        // data). See src/sync/block_propagator.h for the protocol
        // doc.
        else if (type.rfind("block.", 0) == 0) {
            if (!propagator_) {
                reply = {{"req_id", req_id}, {"status", "not_ready"},
                         {"body", nullptr}};
            } else {
                json body_out = propagator_->handle_request(peer_id, type, in);
                reply = {{"req_id", req_id}, {"status", "ok"},
                         {"body",   std::move(body_out)}};
            }
        } else if (type == "songs.list") {
            // Inject the live swarm size next to each chain entry so the
            // client can hide songs nobody is currently serving. The
            // raw HttpServer verb doesn't know about SwarmIndex, so we
            // post-process the JSON here.
            auto [code, body_str] = http_.verb_songs_list();
            if (code == 200) {
                try {
                    auto arr = nlohmann::json::parse(body_str);
                    if (arr.is_array()) {
                        for (auto& s : arr) {
                            const std::string ch_hex =
                                s.value("content_hash", "");
                            Hash256 ch;
                            if (crypto::parse_hash256(ch_hex, ch)) {
                                s["swarm_size"] =
                                    swarm_.members(ch).size();
                            } else {
                                s["swarm_size"] = 0;
                            }
                        }
                        body_str = arr.dump();
                    }
                } catch (const nlohmann::json::exception&) {/* keep raw */}
            }
            reply = wrap_handler_result(req_id, {code, body_str});
        } else if (type == "songs.get") {
            const std::string hash = in.value("content_hash", "");
            reply = wrap_handler_result(req_id, http_.verb_song_get(hash));
        } else if (type == "songs.search") {
            // body may have one of {q, artist, genre}
            if (in.contains("artist")) {
                reply = wrap_handler_result(req_id,
                    http_.verb_songs_search_artist(in.value("artist", "")));
            } else if (in.contains("genre")) {
                reply = wrap_handler_result(req_id,
                    http_.verb_songs_search_genre(in.value("genre", "")));
            } else {
                reply = wrap_handler_result(req_id,
                    http_.verb_songs_search_query(in.value("q", "")));
            }
        } else if (type == "wallet.balance") {
            reply = wrap_handler_result(req_id,
                http_.verb_wallet_balance(in.value("address", "")));
        } else if (type == "wallet.nonce") {
            reply = wrap_handler_result(req_id,
                http_.verb_wallet_nonce(in.value("address", "")));
        } else if (type == "wallet.escrow_balance") {
            reply = wrap_handler_result(req_id,
                http_.verb_wallet_escrow_balance(in.value("address", "")));
        }
        // ---- account registration ---------------------------------------
        //
        // The player signs a UsernameTx preimage locally (sign_message in
        // src/core/transaction.cpp — chain_id || u8 name_len || name ||
        // owner || pubkey || nonce, LE everywhere) and sends it here.
        // Server-side we reconstruct the UsernameTx, run verify_signature
        // against the signing_hash + pubkey, then queue + wake the
        // producer. No password needed — the BIP39 mnemonic is the
        // whole credential, the chain only sees the public address and
        // a signature over it.
        else if (type == "username.register") {
            try {
                const std::string name     = in.value("name", "");
                const std::string addr_hex = in.value("owner_address", "");
                const std::string pk_hex   = in.value("owner_pubkey",  "");
                const uint64_t    nonce    = in.value("nonce", uint64_t{0});
                const std::string sig_hex  = in.value("signature", "");
                if (name.empty() || addr_hex.empty() || pk_hex.empty() ||
                    sig_hex.empty()) {
                    reply = {{"req_id", req_id}, {"status", "invalid"},
                             {"error",  "missing required field"}};
                } else {
                    UsernameTx tx{};
                    tx.name  = name;
                    tx.nonce = nonce;
                    bool malformed = false;
                    if (!crypto::parse_address_checksummed(addr_hex, tx.owner)) malformed = true;
                    auto pk_bytes  = crypto::from_hex(pk_hex);
                    auto sig_bytes = crypto::from_hex(sig_hex);
                    if (pk_bytes.size() != 33 || sig_bytes.size() != 64)
                        malformed = true;
                    if (malformed) {
                        reply = {{"req_id", req_id}, {"status", "invalid"},
                                 {"error",  "bad address / pubkey / signature hex"}};
                    } else {
                        std::copy(pk_bytes.begin(),  pk_bytes.end(),
                                  tx.owner_pubkey.begin());
                        std::copy(sig_bytes.begin(), sig_bytes.end(),
                                  tx.signature.begin());
                        if (!tx.verify_signature()) {
                            reply = {{"req_id", req_id}, {"status", "invalid"},
                                     {"error",  "signature did not verify"}};
                        } else {
                            const auto h   = tx.tx_hash();
                            const auto raw = tx.serialize();
                            if (!db_.put_pending_tx(h, raw)) {
                                reply = {{"req_id", req_id}, {"status", "rejected"},
                                         {"error",  "could not queue tx"}};
                            } else {
                                candidates_.wake();
                                reply = {{"req_id", req_id}, {"status", "ok"},
                                         {"body",   {{"tx_hash",
                                                      crypto::to_hex(h)}}}};
                                std::cout << "[rats-api] username.register: "
                                          << name << " queued for "
                                          << addr_hex.substr(0, 10) << "…\n";
                            }
                        }
                    }
                }
            } catch (const std::exception& e) {
                reply = {{"req_id", req_id}, {"status", "server_error"},
                         {"error",  e.what()}};
            }
        }
        // ---- session control --------------------------------------------
        else if (type == "session.start") {
            // #5 attestation (record-only on the realtime path): derive a
            // device_id from the attested device_key and record its level so
            // the per-device picture covers live sessions, not just offline
            // bundles. AcceptAll never rejects; a real verifier would gate
            // here with no client change. player_address is the fallback key.
            {
                static AcceptAllVerifier g_attest_live;
                AttestationResult att = g_attest_live.verify(
                    in.value("attestation", json::object()),
                    std::string(), in.value("player_address", std::string()));
                record_attestation_level(att.device_id, att.level);
            }
            reply = wrap_handler_result(req_id,
                http_.verb_session_start(in.dump()));
        } else if (type == "session.heartbeat") {
            reply = wrap_handler_result(req_id,
                http_.verb_session_heartbeat(in.value("session_id", ""),
                                             in.dump()));
        } else if (type == "session.complete") {
            reply = wrap_handler_result(req_id,
                http_.verb_session_complete(in.value("session_id", ""),
                                            in.dump()));
        }
        // ---- DB2: wallet-keyed library store ----------------------------
        //
        //   library.delta   { wallet, pubkey, version, ts, add:[hex],
        //                     del:[hex], sig }   -> {applied, version}
        //       A wallet-signed edit. The wallet is the sole writer of its own
        //       record (verified here), so this needs no privilege. The whole
        //       initial library is just a delta with add=everything. On a
        //       genuinely-new version it is applied AND flooded to every peer
        //       (musicchain.library broadcast), which is how an edit clones
        //       onto all nodes as it happens.
        //   library.get     { wallet }          -> {version, hashes:[...]}
        //   library.holders { content_hash }    -> {holders:[wallet,...]}
        else if (type == "library.delta") {
            const bool applied = ingest_library_delta(in.dump(),
                                                      /*broadcast_if_new=*/true);
            reply = {{"req_id", req_id},
                     {"status", applied ? "ok" : "rejected"},
                     {"body", {
                         {"applied", applied},
                         {"version", in.value("version", static_cast<uint64_t>(0))},
                     }}};
            if (!applied) reply["error"] = "bad signature or stale version";
        }
        else if (type == "library.get") {
            Address wallet{};
            if (!crypto::parse_address_checksummed(
                    in.value("wallet", std::string()), wallet)) {
                reply = {{"req_id", req_id}, {"status", "invalid"},
                         {"error", "wallet not a 20-byte address"}};
            } else {
                auto hashes = library_.library(wallet);
                json arr = json::array();
                for (const auto& h : hashes) arr.push_back(crypto::to_hex(h));
                reply = {{"req_id", req_id}, {"status", "ok"},
                         {"body", {
                             {"wallet",  crypto::to_hex(wallet.data(), wallet.size())},
                             {"version", library_.library_version(wallet)},
                             {"hashes",  arr},
                         }}};
            }
        }
        else if (type == "library.holders") {
            Hash256 ch{};
            if (!crypto::parse_hash256(in.value("content_hash", std::string()), ch)) {
                reply = {{"req_id", req_id}, {"status", "invalid"},
                         {"error", "content_hash not 32-byte hex"}};
            } else {
                auto ws = library_.holders(ch);
                json arr = json::array();
                for (const auto& w : ws)
                    arr.push_back(crypto::to_hex(w.data(), w.size()));
                reply = {{"req_id", req_id}, {"status", "ok"},
                         {"body", {
                             {"content_hash", crypto::to_hex(ch)},
                             {"holders",      arr},
                             {"count",        ws.size()},
                         }}};
            }
        }
        // ---- DB2 playlists (ordered; signed; flood-replicated) ----------
        //   playlist.set  { wallet,pubkey,playlist_id,version,ts,deleted,
        //                   name,songs:[hex],sig }  -> {applied,version}
        //   playlist.get  { wallet, playlist_id }   -> {name,version,songs}
        //   playlist.list { wallet }                -> {playlists:[...]}
        else if (type == "playlist.set") {
            const bool applied = ingest_playlist(in.dump(),
                                                 /*broadcast_if_new=*/true);
            reply = {{"req_id", req_id},
                     {"status", applied ? "ok" : "rejected"},
                     {"body", {
                         {"applied", applied},
                         {"version", in.value("version", static_cast<uint64_t>(0))},
                     }}};
            if (!applied) reply["error"] = "bad signature or stale version";
        }
        else if (type == "playlist.get") {
            Address wallet{};
            std::array<uint8_t, 16> pid{};
            if (!crypto::parse_address_checksummed(
                    in.value("wallet", std::string()), wallet) ||
                !from_hex_fixed(in.value("playlist_id", std::string()),
                                pid.data(), pid.size())) {
                reply = {{"req_id", req_id}, {"status", "invalid"},
                         {"error", "bad wallet or playlist_id"}};
            } else {
                auto pl = library_.get_playlist(wallet, pid);
                if (!pl) {
                    reply = {{"req_id", req_id}, {"status", "unknown"},
                             {"error", "no such playlist"}};
                } else {
                    json arr = json::array();
                    for (const auto& h : pl->songs) arr.push_back(crypto::to_hex(h));
                    reply = {{"req_id", req_id}, {"status", "ok"},
                             {"body", {
                                 {"wallet",      crypto::to_hex(wallet.data(), wallet.size())},
                                 {"playlist_id", in.value("playlist_id", std::string())},
                                 {"name",        pl->name},
                                 {"version",     pl->version},
                                 {"songs",       arr},
                             }}};
                }
            }
        }
        else if (type == "playlist.list") {
            Address wallet{};
            if (!crypto::parse_address_checksummed(
                    in.value("wallet", std::string()), wallet)) {
                reply = {{"req_id", req_id}, {"status", "invalid"},
                         {"error", "wallet not a 20-byte address"}};
            } else {
                json arr = json::array();
                for (const auto& pl : library_.list_playlists(wallet)) {
                    json songs = json::array();
                    for (const auto& h : pl.songs) songs.push_back(crypto::to_hex(h));
                    arr.push_back({
                        {"playlist_id", crypto::to_hex(pl.id.data(), pl.id.size())},
                        {"name",        pl.name},
                        {"version",     pl.version},
                        {"count",       pl.songs.size()},
                        {"songs",       songs},
                    });
                }
                reply = {{"req_id", req_id}, {"status", "ok"},
                         {"body", {
                             {"wallet",    crypto::to_hex(wallet.data(), wallet.size())},
                             {"playlists", arr},
                         }}};
            }
        }
        // ---- DB2 anti-entropy: catch a peer up on what it missed ---------
        //   db2.sync { lib:{walletHex:ver}, pl:{walletHex+idHex:ver} }
        //       The requester's summary; we push every record it is behind on
        //       as the normal flooded type (its on_*_cb ingests version-gated).
        else if (type == "db2.sync") {
            const json lib_sum = in.value("lib", json::object());
            const json pl_sum  = in.value("pl",  json::object());
            std::vector<std::pair<const char*, std::string>> to_push;
            library_.for_each_library_payload(
                [&](const Address& w, uint64_t ver, const std::string& payload) {
                    const std::string k = crypto::to_hex(w.data(), w.size());
                    if (ver > lib_sum.value(k, static_cast<uint64_t>(0)))
                        to_push.emplace_back(MC_LIBRARY_TYPE, payload);
                });
            library_.for_each_playlist_payload(
                [&](const Address& w, const std::array<uint8_t, 16>& id,
                    uint64_t ver, const std::string& payload) {
                    const std::string k = crypto::to_hex(w.data(), w.size()) +
                                          crypto::to_hex(id.data(), id.size());
                    if (ver > pl_sum.value(k, static_cast<uint64_t>(0)))
                        to_push.emplace_back(MC_PLAYLIST_TYPE, payload);
                });
            for (const auto& [mt, payload] : to_push)
                if (client_)
                    rats_send_message(client_, peer_id.c_str(), mt, payload.c_str());
            reply = {{"req_id", req_id}, {"status", "ok"},
                     {"body", {{"pushed", to_push.size()}}}};
        }
        // ---- mini-node identity (relay credit attribution) --------------
        //
        // Mini-nodes push `mini.hello` to every fresh full-node peer.
        // The body carries the mini-node's EVM-style wallet address so
        // when the full node later sees a relayed binary-traffic verb
        // (stream.open today; song.audio/song.get pre-pivot) it can
        // credit the right wallet via RelayCreditTracker::increment.
        // Players and non-mini full nodes either don't send mini.hello
        // or send it without a wallet field — both branches are
        // harmless and just leave peer_to_wallet_ empty for them.
        else if (type == "mini.hello") {
            const std::string wallet_hex = in.value("wallet", std::string());
            if (!wallet_hex.empty()) {
                Address addr{};
                // parse_address is case-insensitive hex, so it accepts
                // EIP-55 checksummed forms ("0xAbCd…") as well as plain
                // lowercase. We don't verify the checksum here — a
                // malformed mini-node identity at worst means we lose
                // the credit for that peer's relays until they reconnect
                // with a clean address.
                if (crypto::parse_address_checksummed(wallet_hex, addr)) {
                    std::lock_guard<std::mutex> lk(peer_to_wallet_mu_);
                    peer_to_wallet_[peer_id] = addr;
                    std::cout << "[rats-api] mini.hello: peer "
                              << peer_id.substr(0, 12) << "… wallet "
                              << wallet_hex.substr(0, 10) << "…\n";
                }
            }
            reply = {{"req_id", req_id}, {"status", "ok"},
                     {"body",   json::object()}};
        }
        // ---- audio streaming (swarm only) -------------------------------
        // The full node never holds audio bytes under the post-pivot
        // architecture. stream.open just resolves the swarm so the
        // requester can reach out to a player that has the file.
        else if (type == "stream.open") {
            const std::string hash = in.value("content_hash", "");
            Hash256 ch;
            if (!crypto::parse_hash256(hash, ch)) {
                reply = {{"req_id", req_id},
                         {"status", "invalid"},
                         {"error",  "content_hash not 32-byte hex"}};
            } else if (!db_.get_content_height(ch)) {
                reply = {{"req_id", req_id},
                         {"status", "unknown"},
                         {"error",  "song not on chain"}};
            } else {
                // #10 triangulation: mint a delivery_id + pending-delivery
                // row (this node is the broker), returned so the player
                // threads it through the relay and reports receipt.
                const std::string did_hex = mint_delivery(ch);
                auto members = swarm_.members(ch);
                if (members.empty()) {
                    reply = {{"req_id", req_id},
                             {"status", "ok"},
                             {"body",   {
                                 {"content_hash", hash},
                                 {"peers",        nlohmann::json::array()},
                                 {"source",       "no_swarm"},
                                 {"delivery_id",  did_hex},
                             }}};
                } else {
                    // Variant-aware peers list. Each entry carries the
                    // member's *local* content_hash (the actual bytes it
                    // will serve when stream.open'd), bitrate, and audio
                    // format so the requester can pick a variant — the
                    // streaming path defaults to the lowest bitrate;
                    // download UI surfaces the full list so the user can
                    // pick the quality to keep.
                    nlohmann::json peers_j = nlohmann::json::array();
                    for (const auto& m : members) {
                        peers_j.push_back({
                            {"peer_id",      m.peer_id},
                            {"content_hash", crypto::to_hex(m.content_hash)},
                            {"bitrate",      m.bitrate},
                            {"audio_format", audio_format_to_string(
                                                m.audio_format)},
                        });
                    }
                    reply = {{"req_id", req_id},
                             {"status", "ok"},
                             {"body",   {
                                 {"content_hash", hash},
                                 {"peers",        peers_j},
                                 {"source",       "swarm"},
                                 {"delivery_id",  did_hex},
                             }}};
                }
            }
        }
        // ---- #10 relay-reward triangulation: mini-node report -----------
        // Mini → broker. The mini-node reports how many bytes it relayed for
        // a delivery_id. We credit only on three-way corroboration (broker
        // brokered it + mini reported + player receipted), per-byte.
        else if (type == "relay.report") {
            const std::string ok = handle_relay_report(in) ? "ok" : "ignored";
            reply = {{"req_id", req_id}, {"status", ok}, {"body", json::object()}};
        }
        // ---- #10 relay-reward triangulation: player receipt -------------
        else if (type == "relay.receipt") {
            const std::string ok = handle_relay_receipt(in) ? "ok" : "ignored";
            reply = {{"req_id", req_id}, {"status", ok}, {"body", json::object()}};
        }
        // ---- swarm announcement -----------------------------------------
        // The player computes a fingerprint locally, submits it here.
        // If the chain already has a song with a matching fingerprint
        // hash (i.e. the bytes are the same), the player gets the
        // existing content_hash back and is recorded as a swarm peer
        // for that song. Subsequent stream.open's for that hash can
        // return this peer in the swarm fallback above.
        else if (type == "fingerprint.submit") {
            // Accept either a full compressed fingerprint blob (which we
            // hash here) or a precomputed `fingerprint_hash` (32-byte
            // hex) the client already hashed itself. The hash form is
            // ~64 bytes vs ~400 KB and is what real players will use
            // once they fingerprint locally with Chromaprint/Dejavu.
            const std::string fp     = in.value("fingerprint",      "");
            const std::string fp_hex = in.value("fingerprint_hash", "");
            const std::string my_pid = in.value("peer_id",          "");
            const std::string origin = env.value("originator_peer_id",
                                                 std::string());
            const std::string announcing_pid =
                !my_pid.empty() ? my_pid
                : !origin.empty() ? origin
                : peer_id;

            Hash256 fph{};
            bool have_hash = false;
            // Bug fix: ALWAYS recompute fingerprint_hash from the
            // actual compressed_fingerprint bytes when we have them.
            // If the player only sent the hash, we trust it but it
            // can't be used for new-song registration — only for
            // existence lookups. This stops a class of bug where the
            // player's claimed fph disagreed with sha256(compressed)
            // and Block::validate later rejected the block (because
            // Block::validate also recomputes), wedging the producer.
            if (!fp.empty()) {
                fph = crypto::sha256(
                    reinterpret_cast<const uint8_t*>(fp.data()), fp.size());
                have_hash = true;
            } else if (!fp_hex.empty()) {
                if (!crypto::parse_hash256(fp_hex, fph)) {
                    reply = {{"req_id", req_id},
                             {"status", "invalid"},
                             {"error",  "fingerprint_hash not 32-byte hex"}};
                } else {
                    have_hash = true;
                }
            } else {
                reply = {{"req_id", req_id},
                         {"status", "invalid"},
                         {"error",  "need fingerprint or fingerprint_hash"}};
            }

            if (have_hash) {
                auto match = db_.get_content_hash_for_fingerprint(fph);
                // Fallback: if the fingerprint_hash index missed but the
                // player included a content_hash that the chain already
                // knows, treat that as the match. This covers the case
                // where the index was rebuilt from compressed bytes whose
                // hash no longer agrees with the player's stored hash
                // (e.g. chromaprint serialization changed between
                // versions, or the player stored the digest before the
                // index was rebuilt). Without it those songs sit on chain
                // forever with no swarm peer, so stream.open returns no
                // serving player.
                if (!match) {
                    const std::string ch_hex = in.value("content_hash", "");
                    Hash256 ch{};
                    if (!ch_hex.empty() && crypto::parse_hash256(ch_hex, ch)
                        && db_.get_content_height(ch)) {
                        match = ch;
                    }
                }
                // Fuzzy chromaprint match: two different encodings of the
                // same song produce different fingerprint_hashes (sha256
                // of the compressed blob) and different content_hashes
                // (sha256 of the bytes), so the exact-hash branches above
                // both miss. Decode the submitted fingerprint and probe
                // the bucket index for candidates whose chromaprint
                // hashes line up. Anything scoring above kSimThreshold
                // counts as the same song — swarm join, no duplicate
                // block.
                if (!match && !fp.empty()) {
                    // Shared with chain.cpp's replay dup-check (see
                    // fingerprint.h) — these MUST stay equal or nodes fork.
                    const float kSimThreshold = audio::kChromaprintSimThreshold;
                    auto submitted =
                        audio::Fingerprint::from_compressed(fp);
                    if (!submitted) {
                        std::cout << "[rats-api] fuzzy skip: submitter's "
                                     "compressed fingerprint failed to "
                                     "decode (len=" << fp.size() << ")\n";
                    } else {
                        std::unordered_set<std::string>      seen;
                        std::pair<Hash256, float> best{{}, 0.0f};
                        int n_candidates = 0;
                        bool found = false;   // early-exit on first >= threshold
                        for (auto bucket : submitted->bucket_ids()) {
                            for (const auto& cand :
                                 db_.get_bucket(bucket)) {
                                const auto cand_hex =
                                    crypto::to_hex(cand);
                                if (!seen.insert(cand_hex).second) continue;
                                ++n_candidates;
                                auto entry = db_.get_fingerprint(cand);
                                if (!entry) continue;
                                auto cand_fp =
                                    audio::Fingerprint::from_compressed(
                                        entry->compressed_fingerprint);
                                if (!cand_fp) continue;
                                const float sim =
                                    submitted->similarity(*cand_fp);
                                if (sim > best.second) {
                                    best.first  = cand;
                                    best.second = sim;
                                }
                                // Early-exit: the swarm-join result only needs
                                // ANY candidate >= threshold (it overrides the
                                // swarm key with the player's local content_hash
                                // below), not the global argmax. Mirrors the
                                // consensus path (chain.cpp), which already
                                // breaks on the first duplicate. Turns the
                                // common duplicate case from a full candidate
                                // scan into first-hit.
                                if (sim >= kSimThreshold) {
                                    found = true;
                                    break;
                                }
                            }
                            if (found) break;
                        }
                        std::cout << "[rats-api] fuzzy probe: "
                                  << n_candidates << " candidates, best="
                                  << best.second << " (threshold "
                                  << kSimThreshold << ")\n";
                        if (best.second >= kSimThreshold) {
                            match = best.first;
                            std::cout << "[rats-api] fuzzy match: sim="
                                      << best.second << " "
                                      << announcing_pid.substr(0, 12)
                                      << " -> "
                                      << crypto::to_hex(best.first)
                                             .substr(0, 12) << "\n";
                        }
                    }
                }
                if (match) {
                    // Build the announce with the player's *local*
                    // content_hash (often equal to canonical, but for
                    // fuzzy-matched variants will be the local file's
                    // own SHA-256). Bitrate + format identify the
                    // variant so requesters can pick a quality.
                    store::SwarmMember member;
                    member.peer_id      = announcing_pid;
                    Hash256 local_ch{};
                    const std::string local_ch_hex =
                        in.value("content_hash", "");
                    if (!local_ch_hex.empty() &&
                        crypto::parse_hash256(local_ch_hex, local_ch)) {
                        member.content_hash = local_ch;
                    } else {
                        member.content_hash = *match;
                    }
                    member.bitrate      = static_cast<uint32_t>(
                        in.value("bitrate", 0));
                    member.audio_format = audio_format_from_string(
                        in.value("audio_format", std::string("ogg")));
                    swarm_.announce(*match, member);
                    // (DHT un-nerf P1) The full node no longer announces
                    // ITSELF as a DHT seeder for the content_hash. Post-pivot
                    // it holds no audio bytes, and the player's DHT-discovery
                    // path now dials DHT results as audio.piece_get byte
                    // sources — so a self-announce just lures consumers into
                    // dialing a node that serves nothing (wasted attempt +
                    // ban). The full node is already reachable as a swarm
                    // tracker via the VPS bootstrap + routes.get, so the DHT
                    // tracker role is redundant. Only players that actually
                    // hold the bytes announce (SwarmRegistry.announceOnly).
                    reply = {{"req_id", req_id},
                             {"status", "ok"},
                             {"body", {
                                 {"matched",      true},
                                 {"content_hash", crypto::to_hex(*match)},
                                 {"swarm_size",   swarm_.members(*match).size()},
                             }}};
                    std::cout << "[rats-api] swarm join: "
                              << announcing_pid.substr(0, 12)
                              << " -> " << crypto::to_hex(*match).substr(0, 12)
                              << " (" << member.bitrate << " bps "
                              << audio_format_to_string(member.audio_format)
                              << ")\n";
                } else {
                    // No match. If the player included the full fingerprint
                    // blob + metadata, treat this as a new-song registration
                    // and queue it for the next block. The player auto-joins
                    // the swarm so future requesters can fetch from them.
                    const std::string ch_hex = in.value("content_hash", "");
                    Hash256 ch{};
                    const bool can_register =
                        !fp.empty() &&
                        !ch_hex.empty() &&
                        crypto::parse_hash256(ch_hex, ch);
                    if (!can_register) {
                        reply = {{"req_id", req_id},
                                 {"status", "ok"},
                                 {"body", {
                                     {"matched",          false},
                                     {"registered",       false},
                                     {"fingerprint_hash", crypto::to_hex(fph)},
                                 }}};
                    } else {
                        PendingRegistration reg;
                        reg.content_hash             = ch;
                        reg.fingerprint_hash         = fph;
                        reg.compressed_fingerprint   = fp;
                        reg.audio_format = audio_format_from_string(
                            in.value("audio_format", std::string("ogg")));
                        reg.duration_ms              = in.value("duration_ms", 0);
                        reg.title                    = in.value("title",  std::string());
                        reg.artist                   = in.value("artist", std::string());
                        reg.genre                    = in.value("genre",  std::string());
                        reg.album                    = in.value("album",  std::string());
                        // Optional ID3 numerics. Submitters that don't
                        // have them just send 0, which we treat as "not
                        // present" everywhere downstream.
                        reg.year                     = static_cast<uint16_t>(
                            in.value("year",         0));
                        reg.track_number             = static_cast<uint16_t>(
                            in.value("track_number", 0));
                        // artist_address optional — leave zero if absent.
                        // Accept any form parse_address() handles: bare
                        // 40-hex, 0x-prefixed, EIP-55 mixed-case. Older
                        // clients that omit the field land at zero and
                        // the artist share routes through the unclaimed
                        // escrow (see compute_mint_outputs).
                        const std::string aa_hex = in.value("artist_address",
                                                            std::string());
                        if (!aa_hex.empty()) {
                            Address parsed{};
                            if (crypto::parse_address_checksummed(aa_hex, parsed)) {
                                reg.artist_address = parsed;
                            }
                        }
                        reg.announcing_peer_id       = announcing_pid;
                        // Capture display fields BEFORE std::move(reg) — the
                        // moved-from object's strings are unspecified.
                        const std::string log_title  = reg.title;
                        const std::string log_artist = reg.artist;
                        // Take bitrate before we move reg so it makes it
                        // into the SwarmMember below too.
                        const uint32_t reg_bitrate = static_cast<uint32_t>(
                            in.value("bitrate", 0));
                        const AudioFormat reg_format = reg.audio_format;
                        const bool queued = candidates_.enqueue_registration(
                            std::move(reg));
                        // Pre-emptively announce so swarm.locate already
                        // returns the announcing peer before block lands.
                        store::SwarmMember self_member;
                        self_member.peer_id      = announcing_pid;
                        self_member.content_hash = ch; // local == canonical for the first submitter
                        self_member.bitrate      = reg_bitrate;
                        self_member.audio_format = reg_format;
                        swarm_.announce(ch, self_member);
                        // (DHT un-nerf P1) full node no longer self-announces
                        // as a DHT seeder — see the matched-branch note above.
                        // It holds no bytes; only the holding player announces.
                        reply = {{"req_id", req_id},
                                 {"status", "ok"},
                                 {"body", {
                                     {"matched",          false},
                                     {"registered",       queued},
                                     {"content_hash",     crypto::to_hex(ch)},
                                     {"fingerprint_hash", crypto::to_hex(fph)},
                                     {"swarm_size",       swarm_.members(ch).size()},
                                 }}};
                        std::cout << "[rats-api] register: \""
                                  << log_title << "\" by " << log_artist
                                  << " from " << announcing_pid.substr(0, 12)
                                  << " (queued=" << queued << ")\n";
                    }
                }
            }
        }
        // ---- efficient swarm protocol ----------------------------------
        //
        // The pre-2026-06-13 path had every player rebroadcast every
        // file's fingerprint at every launch — bandwidth scaled
        // linearly with library size. The new verbs let the player
        // send the chain-canonical content_hash set ONCE per session
        // and then deltas, with a cheap "digest matches?" preflight
        // so unchanged libraries cost a single 96-byte round-trip.
        //
        // * swarm.hello_digest { peer_id?, digest, count } →
        //     {match: bool, peer_size}      // bool only; no list
        // * swarm.hello       { peer_id?, hashes: [hex...] } →
        //     {peer_size, unknown:[hex...]} // full node persists the
        //                                   // new set; unknown is the
        //                                   // hashes the chain hasn't
        //                                   // seen yet — player still
        //                                   // calls fingerprint.submit
        //                                   // for those.
        // * swarm.add    { peer_id?, hashes: [hex...] }   // delta
        // * swarm.remove { peer_id?, hashes: [hex...] }   // delta
        //
        // peer_id is optional — the rats originator (or sender) is the
        // default, same convention as fingerprint.submit.
        else if (type == "swarm.hello_digest") {
            const std::string my_pid = in.value("peer_id", "");
            const std::string origin = env.value("originator_peer_id",
                                                  std::string());
            const std::string pid = !my_pid.empty() ? my_pid
                                  : !origin.empty() ? origin
                                                    : peer_id;
            // Receipt of any swarm RPC proves the peer is online — the
            // direct connect/disconnect callbacks above only fire for
            // peers connected to us directly, but most players come in
            // through a relay so their online-state has to be inferred
            // from message arrival here.
            swarm_.mark_peer_online(pid);
            const std::string client_digest = in.value("digest", "");
            const std::string server_digest = swarm_.peer_digest(pid);
            const bool match = !client_digest.empty()
                            && client_digest == server_digest;
            reply = {{"req_id", req_id},
                     {"status", "ok"},
                     {"body", {
                         {"match",         match},
                         {"server_digest", server_digest},
                         {"peer_size",     swarm_.peer_size(pid)},
                     }}};
            if (match) {
                // Re-arm the TTL safety net so a clock-skew quirk
                // doesn't expire a known-online peer's entries.
                swarm_.touch_peer(pid);
            }
        }
        else if (type == "swarm.hello") {
            const std::string my_pid = in.value("peer_id", "");
            const std::string origin = env.value("originator_peer_id",
                                                  std::string());
            const std::string pid = !my_pid.empty() ? my_pid
                                  : !origin.empty() ? origin
                                                    : peer_id;
            swarm_.mark_peer_online(pid);
            const auto& hashes_json = in.value("hashes", json::array());
            std::vector<std::pair<Hash256, store::SwarmMember>> members;
            std::vector<std::string> unknown;
            members.reserve(hashes_json.is_array() ? hashes_json.size() : 0);
            if (hashes_json.is_array()) {
                for (const auto& h : hashes_json) {
                    if (!h.is_string()) continue;
                    const std::string hash_hex = h.get<std::string>();
                    Hash256 ch{};
                    if (!crypto::parse_hash256(hash_hex, ch)) continue;
                    // Hashes the chain doesn't recognize are returned
                    // to the player so it knows to follow up with
                    // fingerprint.submit for the actual block ingest.
                    if (!db_.get_content_height(ch)) {
                        unknown.push_back(hash_hex);
                        continue;
                    }
                    store::SwarmMember m;
                    m.peer_id      = pid;
                    m.content_hash = ch;
                    members.emplace_back(ch, m);
                }
            }
            const std::string new_digest = swarm_.replace_peer(pid, members);
            reply = {{"req_id", req_id},
                     {"status", "ok"},
                     {"body", {
                         {"peer_size",     swarm_.peer_size(pid)},
                         {"server_digest", new_digest},
                         {"unknown",       unknown},
                     }}};
            std::cout << "[rats-api] swarm.hello from "
                      << pid.substr(0, 12) << " → " << members.size()
                      << " known, " << unknown.size() << " unknown\n";
        }
        else if (type == "swarm.add" || type == "swarm.remove") {
            const std::string my_pid = in.value("peer_id", "");
            const std::string origin = env.value("originator_peer_id",
                                                  std::string());
            const std::string pid = !my_pid.empty() ? my_pid
                                  : !origin.empty() ? origin
                                                    : peer_id;
            swarm_.mark_peer_online(pid);
            const auto& hashes_json = in.value("hashes", json::array());
            std::vector<std::string> unknown;
            size_t applied = 0;
            if (hashes_json.is_array()) {
                for (const auto& h : hashes_json) {
                    if (!h.is_string()) continue;
                    Hash256 ch{};
                    if (!crypto::parse_hash256(h.get<std::string>(), ch)) {
                        continue;
                    }
                    if (type == "swarm.add") {
                        if (!db_.get_content_height(ch)) {
                            unknown.push_back(h.get<std::string>());
                            continue;
                        }
                        store::SwarmMember m;
                        m.peer_id      = pid;
                        m.content_hash = ch;
                        swarm_.announce(ch, m);
                        ++applied;
                    } else {
                        swarm_.drop(ch, pid);
                        ++applied;
                    }
                }
            }
            reply = {{"req_id", req_id},
                     {"status", "ok"},
                     {"body", {
                         {"applied",       applied},
                         {"unknown",       unknown},
                         {"peer_size",     swarm_.peer_size(pid)},
                         {"server_digest", swarm_.peer_digest(pid)},
                     }}};
        }
        // Mini-node-forwarded notification that one of its connected
        // peers (a player, usually) is online via rats. Used both:
        //   * when a player first connects through a mini-node
        //   * when a full node reconnects to a mini-node, the
        //     mini-node replays the currently-online player set so
        //     the full node's swarm view recovers without waiting for
        //     every player to re-send swarm.hello on its next tick.
        // Body: {peer_id}.
        else if (type == "swarm.peer_online") {
            const std::string online_pid = in.value("peer_id", "");
            if (!online_pid.empty()) {
                swarm_.mark_peer_online(online_pid);
            }
            reply = {{"req_id", req_id},
                     {"status", "ok"},
                     {"body", {{"peer_id", online_pid}}}};
        }
        // Mini-node-forwarded notification that one of its connected
        // peers (a player, almost always) has dropped the rats link.
        // The full node has no direct callback for relayed peers, so
        // this is how we learn to flip their entries to "offline" and
        // hide their songs from the Discover surface in real time.
        // Body: {peer_id}.
        else if (type == "swarm.peer_offline") {
            const std::string offline_pid = in.value("peer_id", "");
            if (!offline_pid.empty()) {
                swarm_.mark_peer_offline(offline_pid);
                std::cout << "[rats-api] swarm.peer_offline "
                          << offline_pid.substr(0, 12)
                          << " (via " << peer_id.substr(0, 12) << ")\n";
            }
            reply = {{"req_id", req_id},
                     {"status", "ok"},
                     {"body", {{"peer_id", offline_pid}}}};
        }
        // ---- swarm leave -----------------------------------------------
        // Inverse of fingerprint.submit's swarm join: when a player
        // removes a file (folder un-added from My Library), they tell
        // the full node to drop them from the swarm so requesters
        // don't waste a round-trip to a peer that no longer has the bytes.
        //
        // Body: {content_hash, peer_id?}   peer_id falls back to envelope
        //                                  originator → immediate sender.
        // Reply: {body: {dropped: bool, content_hash, swarm_size}}
        else if (type == "swarm.leave") {
            const std::string hash_hex = in.value("content_hash", "");
            const std::string my_pid   = in.value("peer_id",      "");
            const std::string origin   = env.value("originator_peer_id",
                                                   std::string());
            const std::string leaver_pid =
                !my_pid.empty() ? my_pid
                : !origin.empty() ? origin
                : peer_id;
            Hash256 ch{};
            if (!crypto::parse_hash256(hash_hex, ch)) {
                reply = {{"req_id", req_id},
                         {"status", "invalid"},
                         {"error",  "content_hash not 32-byte hex"}};
            } else {
                swarm_.drop(ch, leaver_pid);
                reply = {{"req_id", req_id},
                         {"status", "ok"},
                         {"body", {
                             {"dropped",      true},
                             {"content_hash", hash_hex},
                             {"swarm_size",   swarm_.members(ch).size()},
                         }}};
                std::cout << "[rats-api] swarm.leave: "
                          << leaver_pid.substr(0, 12) << " left "
                          << hash_hex.substr(0, 12) << "\n";
            }
        }
        // ---- Moderation log sync ----------------------------------------
        //
        // A newly-connecting peer sends its latest known mod-log timestamp
        // here; we push every envelope strictly newer than that as
        // separate `musicchain.mod` messages. The reply just acknowledges
        // the count so the requester can log progress.
        else if (type == "mod.sync_since") {
            const uint64_t since = in.value("since_ts_ms", 0ULL);
            push_mod_log_since(peer_id, since);
            reply = {{"req_id", req_id}, {"status", "ok"},
                     {"body",   {{"since_ts_ms", since}}}};
        }
        // ---- DMCA + KYC inbox uploads -----------------------------------
        //
        // Both verbs follow the same pattern: caller submits a base64'd
        // file, the full node sanitizes the filename and drops the bytes
        // into <data_dir>/<inbox>/<utc-ts>_<safe-name>. The moderator's
        // TUI lists each inbox on F1. No signature required at upload
        // time; human review at the TUI is the gate. Hard cap on size
        // (32 MB) so a buggy client can't OOM the full node.
        else if (type == "dmca.submit_pdf" || type == "kyc.submit_form") {
            const bool is_kyc = (type == "kyc.submit_form");
            const char* inbox_name = is_kyc ? "kyc" : "dmca";
            const char* default_name = is_kyc ? "kyc_form.pdf" : "takedown.pdf";

            const std::string in_name = in.value("filename",   std::string());
            const std::string b64     = in.value("content_b64", std::string());
            if (b64.empty()) {
                reply = {{"req_id", req_id}, {"status", "invalid"},
                         {"error",  "content_b64 missing"}};
            } else if (b64.size() > 64u * 1024u * 1024u) {
                reply = {{"req_id", req_id}, {"status", "invalid"},
                         {"error",  "payload too large (>32 MB)"}};
            } else {
                auto bytes = mc::audio::base64_decode(b64);
                std::string safe;
                for (char c : in_name) {
                    if (c == '/' || c == '\\' || c == ':' || c == '\0') continue;
                    if (c == '.' || c == '_' || c == '-' || c == ' ' ||
                        (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9'))
                        safe.push_back(c);
                }
                while (!safe.empty() && safe.front() == '.') safe.erase(0, 1);
                if (safe.empty()) safe = default_name;
                // KYC accepts pdf/jpg/jpeg/png so artists can snap a
                // photo of their ID � only force a .pdf default. DMCA
                // is PDF-only and we coerce the extension.
                if (!is_kyc) {
                    if (safe.size() < 4 ||
                        (safe.substr(safe.size() - 4) != ".pdf" &&
                         safe.substr(safe.size() - 4) != ".PDF"))
                        safe += ".pdf";
                }

                const auto now = std::chrono::system_clock::now();
                const auto t   = std::chrono::system_clock::to_time_t(now);
                std::tm tm{};
#ifdef _WIN32
                gmtime_s(&tm, &t);
#else
                gmtime_r(&t, &tm);
#endif
                char ts[32];
                std::snprintf(ts, sizeof(ts), "%04d%02d%02d_%02d%02d%02d_",
                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                              tm.tm_hour, tm.tm_min, tm.tm_sec);

                // KYC drops include the submitter's wallet address so the
                // moderator can match the form to the escrow they're
                // about to release. Embed the full 40-hex address in the
                // filename so the TUI review screen can pull the payout
                // target without keeping a separate sidecar.
                const std::string from_addr = in.value("from_address",
                                                        std::string());
                std::string prefix(ts);
                if (is_kyc && !from_addr.empty()) {
                    std::string addr_safe;
                    for (char c : from_addr) {
                        if ((c >= 'a' && c <= 'f') ||
                            (c >= 'A' && c <= 'F') ||
                            (c >= '0' && c <= '9'))
                            addr_safe.push_back(c);
                    }
                    if (addr_safe.size() == 40) {
                        prefix += addr_safe;
                        prefix += "_";
                    }
                }

                std::filesystem::path inbox =
                    std::filesystem::path(config_.data_dir) / inbox_name;
                std::error_code ec;
                std::filesystem::create_directories(inbox, ec);
                const auto path = inbox / (prefix + safe);

                // Phase 4: encrypt the inbox file to every currently
                // active moderator on chain. Any moderator can later
                // decrypt with their own private key; the node
                // operator cannot read the contents without being a
                // moderator. If no moderator pubkeys are known (no
                // founder yet, or DB corruption) we fall back to
                // storing the plaintext so the operator can still see
                // submissions during bootstrap — early-chain operators
                // are expected to be the founder anyway.
                std::vector<std::pair<Address, PubKey33>> recipients;
                for (const auto& a : db_.list_active_moderators()) {
                    auto pk = db_.get_mod_pubkey(a);
                    if (pk.has_value()) recipients.emplace_back(a, *pk);
                }

                std::vector<uint8_t> blob_to_write = bytes;
                std::string suffix_marker;
                if (!recipients.empty()) {
                    auto encrypted = mc::crypto::ecies_encrypt(bytes, recipients);
                    if (!encrypted.empty()) {
                        blob_to_write = std::move(encrypted);
                        suffix_marker = ".enc";
                    } else {
                        std::cerr << "[rats-api] ecies_encrypt failed for "
                                  << recipients.size()
                                  << " recipients; storing plaintext\n";
                    }
                }

                auto final_path = path;
                if (!suffix_marker.empty()) {
                    final_path += suffix_marker;
                }

                std::ofstream f(final_path, std::ios::binary | std::ios::trunc);
                if (!f) {
                    reply = {{"req_id", req_id}, {"status", "error"},
                             {"error",  "could not open inbox file"}};
                } else {
                    f.write(reinterpret_cast<const char*>(blob_to_write.data()),
                            (std::streamsize)blob_to_write.size());
                    f.close();
                    std::cout << "[rats-api] " << type << ": stored "
                              << blob_to_write.size() << " bytes as "
                              << final_path.filename().string()
                              << " (recipients=" << recipients.size() << ")\n";
                    reply = {{"req_id", req_id}, {"status", "ok"},
                             {"body", {
                                 {"stored_as", path.filename().string()},
                                 {"size",      bytes.size()},
                             }}};
                }
            }
        }
        // ---- offline play-proof bundle ---------------------------------
        //
        // The player sends one of these on reconnect after running
        // sessions while disconnected. The bundle's wire format is
        // documented in docs/offline_play_proof.md in the player repo;
        // the home node:
        //   (a) verifies the ECDSA signature against the embedded
        //       pubkey and checks that the pubkey derives to
        //       player_address,
        //   (b) runs a battery of bot heuristics on the sensor /
        //       transition signals (stubbed below — each is a separate
        //       function so the thresholds can be tuned independently),
        //   (c) for sessions that pass, replays them through the
        //       existing session.start / session.heartbeat /
        //       session.complete pipeline so the mint logic, royalty
        //       splits, and dedup checks (db_.is_session_used) are
        //       shared with the online path.
        //
        // Reply shape:
        //   { status: "ok"|"rejected",
        //     body: { accepted: int,
        //             rejected: [{session_id, reason}],
        //             flagged_patterns: [string...] } }
        //
        // The TODOs in this verb are the bot heuristics named in the
        // design doc. They currently always pass; the verb still
        // executes the credit pipeline end-to-end so the wire path is
        // exercisable.
        else if (type == "offline.play_proof.submit") {
            const std::string player_addr_hex = in.value("player_address", "");
            const std::string pubkey_hex      = in.value("pubkey",         "");
            const std::string sig_hex         = in.value("signature",      "");
            const std::string nonce_hex       = in.value("bundle_nonce",   "");
            const uint64_t    created_at_ms   = in.value("created_at_ms", 0ULL);

            // ---- 1. signature verification ---------------------------
            Address player_addr{};
            PubKey33 pubkey_compressed{};
            Sig64    sig{};
            bool gate_ok = true;
            std::string gate_err;
            if (!crypto::parse_address_checksummed(player_addr_hex, player_addr)) {
                gate_ok = false; gate_err = "bad player_address";
            }
            if (gate_ok) {
                auto pk_bytes = crypto::from_hex(pubkey_hex);
                if (pk_bytes.size() != pubkey_compressed.size()) {
                    gate_ok = false; gate_err = "pubkey must be 33-byte hex";
                } else {
                    std::copy(pk_bytes.begin(), pk_bytes.end(),
                              pubkey_compressed.begin());
                    // Pubkey must derive to the claimed address — stops a
                    // sender from signing a bundle on behalf of a different
                    // wallet.
                    if (crypto::address_from_pubkey(pubkey_compressed)
                        != player_addr) {
                        gate_ok = false;
                        gate_err = "pubkey does not derive to player_address";
                    }
                }
            }
            if (gate_ok) {
                auto sig_bytes = crypto::from_hex(sig_hex);
                if (sig_bytes.size() != sig.size()) {
                    gate_ok = false; gate_err = "signature must be 64-byte hex";
                } else {
                    std::copy(sig_bytes.begin(), sig_bytes.end(), sig.begin());
                }
            }
            if (gate_ok) {
                // Canonical-JSON the body without `signature` so the
                // signed bytes match what the player computed in
                // OfflineSubmitService._canonicalJson(): object keys
                // sorted ascending lexicographically, no whitespace,
                // arrays preserve order, primitives JSON-encoded by
                // nlohmann's default serializer.
                json body_for_sig = in;
                body_for_sig.erase("signature");
                std::function<void(const json&, std::string&)> emit =
                    [&](const json& j, std::string& out) {
                        if (j.is_object()) {
                            std::vector<std::string> keys;
                            keys.reserve(j.size());
                            for (auto it = j.begin(); it != j.end(); ++it) {
                                keys.push_back(it.key());
                            }
                            std::sort(keys.begin(), keys.end());
                            out.push_back('{');
                            bool first = true;
                            for (const auto& k : keys) {
                                if (!first) out.push_back(',');
                                first = false;
                                out += json(k).dump();
                                out.push_back(':');
                                emit(j.at(k), out);
                            }
                            out.push_back('}');
                        } else if (j.is_array()) {
                            out.push_back('[');
                            bool first = true;
                            for (const auto& el : j) {
                                if (!first) out.push_back(',');
                                first = false;
                                emit(el, out);
                            }
                            out.push_back(']');
                        } else {
                            out += j.dump();
                        }
                    };
                std::string canonical;
                emit(body_for_sig, canonical);
                const Hash256 digest = crypto::sha256(
                    reinterpret_cast<const uint8_t*>(canonical.data()),
                    canonical.size());
                if (!crypto::verify_ecdsa(digest, sig, pubkey_compressed)) {
                    gate_ok = false;
                    gate_err = "signature verify failed";
                }
            }
            if (!gate_ok) {
                reply = {{"req_id", req_id}, {"status", "rejected"},
                         {"error",  gate_err}};
                send_reply(peer_id, reply.dump());
                return;
            }

            // ---- 2. replay protection (cross-restart, persisted) ------
            // A replayed bundle is byte-identical, so its 64-byte
            // signature is a stable unique key. Persist "obp:<sig_hex>"
            // to leveldb so the same bundle can't be credited twice,
            // surviving full-node restarts. (Per-session `u:` markers in
            // the credit pipeline also defend double-credit; this rejects
            // the whole bundle up front and closes the old TODO gap.)
            const std::string replay_key = "obp:" + sig_hex;
            if (db_.get(replay_key).has_value()) {
                reply = {{"req_id", req_id}, {"status", "rejected"},
                         {"error",  "bundle already submitted (replay)"}};
                send_reply(peer_id, reply.dump());
                return;
            }
            (void)nonce_hex;
            (void)created_at_ms;

            const std::string addr_hex = crypto::to_hex(player_addr.data(), 20);

            // ---- 2b. device attestation (#5, Axis-A) -----------------
            // Default verifier accepts and derives a stable device_id from
            // the attested device_key (or the legacy device_id field). A
            // real hardware verifier (Play Integrity / App Attest / TPM)
            // swaps in here with NO downstream change — the device_id it
            // returns is what DeviceIDChurn and the per-device history key
            // on, automatically tightening from a forgeable id to a
            // hardware-bound one. See device_attestation.h.
            static AcceptAllVerifier g_attest;
            AttestationResult att = g_attest.verify(
                in.value("attestation", json::object()),
                in.value("device_id", std::string()), addr_hex);
            if (!att.ok) {
                reply = {{"req_id", req_id}, {"status", "rejected"},
                         {"error", "device attestation failed: " + att.reason}};
                send_reply(peer_id, reply.dump());
                return;
            }
            record_attestation_level(att.device_id, att.level);  // #5 record

            // ---- 3. bot-detection heuristics -------------------------
            // Real deterministic checks over the bundle. HARD rules (high
            // confidence) reject the whole bundle; SOFT rules (noisy /
            // proxy / client-stubbed signals) only annotate
            // flagged_patterns. See docs §22.6 and the offline_play_proof
            // spec. Inputs are parsed defensively — fields absent or
            // stubbed to a sentinel cause the relevant rule to no-op, so
            // an honest bundle is never falsely rejected.
            std::vector<std::string> flagged_patterns;
            bool bot_reject = false;
            auto soft_flag = [&](const char* n){ flagged_patterns.emplace_back(n); };
            auto hard_flag = [&](const char* n){ flagged_patterns.emplace_back(n); bot_reject = true; };

            const auto& h_sessions   = in.value("sessions", json::array());
            const uint64_t wall_base = in.value("wall_base_ms", (uint64_t)0);
            const std::string device_id_hex = att.device_id;  // attested (or fallback) id
            constexpr uint64_t k1h = 60ULL*60*1000, k4h = 4*k1h;

            // Bundle wall span across all sessions (for the span-based rules).
            uint64_t span_lo = UINT64_MAX, span_hi = 0;
            for (const auto& s : h_sessions) {
                if (!s.is_object()) continue;
                uint64_t sw = s.value("started_wall_ms", (uint64_t)0);
                uint64_t ew = s.value("ended_wall_ms",   (uint64_t)0);
                if (sw && sw < span_lo) span_lo = sw;
                if (ew > span_hi)       span_hi = ew;
            }
            if (wall_base && wall_base < span_lo) span_lo = wall_base;
            const uint64_t bundle_span =
                (span_hi > span_lo && span_lo != UINT64_MAX) ? span_hi - span_lo : 0;

            // (1) PerfectIntervalHeartbeats, (2) MonotonicClockJumps,
            // (3) HeartbeatDensityTooHigh — per-session timing.
            for (const auto& s : h_sessions) {
                if (!s.is_object()) continue;
                const auto& beats = s.value("heartbeats", json::array());
                std::vector<uint64_t> wall, mono;
                for (const auto& b : beats) {
                    if (!b.is_object()) continue;
                    wall.push_back(b.value("wall_ms",      (uint64_t)0));
                    mono.push_back(b.value("monotonic_ms", (uint64_t)0));
                }
                // (2) monotonic non-decreasing AND tracks wall within ±2 s.
                for (size_t i = 1; i < mono.size(); ++i) {
                    if (mono[i] < mono[i-1]) { hard_flag("MonotonicClockJumps"); break; }
                    long long dmono = (long long)mono[i] - (long long)mono[0];
                    long long dwall = (long long)wall[i] - (long long)wall[0];
                    long long diff  = dmono - dwall; if (diff < 0) diff = -diff;
                    if (diff > 2000) { hard_flag("MonotonicClockJumps"); break; }
                }
                // (1) perfect intervals: gap variance < 2500 (=50ms σ) over
                //     >=30 beats, on BOTH clocks (wall is user-controllable).
                auto gap_var = [](const std::vector<uint64_t>& v)->double{
                    if (v.size() < 2) return 1e18;
                    double m = 0; size_t n = v.size()-1;
                    for (size_t i=1;i<v.size();++i) m += (double)v[i]-(double)v[i-1];
                    m /= n;
                    double s2 = 0;
                    for (size_t i=1;i<v.size();++i){ double g=(double)v[i]-(double)v[i-1]; s2+=(g-m)*(g-m);}
                    return s2 / n;
                };
                if (mono.size() >= 30 && gap_var(mono) < 2500.0 && gap_var(wall) < 2500.0)
                    hard_flag("PerfectIntervalHeartbeats");
                // (3) density: heartbeats per song-second > 12.
                uint64_t dur = s.value("song_duration_ms", (uint64_t)0);
                if (dur == 0) {
                    uint64_t sw = s.value("started_wall_ms",(uint64_t)0);
                    uint64_t ew = s.value("ended_wall_ms",  (uint64_t)0);
                    dur = (ew > sw) ? ew - sw : 0;
                }
                if (dur > 0) {
                    if ((double)beats.size() / (dur/1000.0) > 12.0)
                        hard_flag("HeartbeatDensityTooHigh");
                } else if (beats.size() > 1) {
                    hard_flag("HeartbeatDensityTooHigh");
                }
            }

            // (4) ImplausibleSessionConcurrency: different content_hashes
            //     overlapping in monotonic time within one bundle (>2 s).
            {
                struct Iv { uint64_t lo, hi; std::string ch; };
                std::vector<Iv> ivs;
                for (const auto& s : h_sessions) {
                    if (!s.is_object()) continue;
                    uint64_t lo = s.value("started_monotonic_ms",(uint64_t)0);
                    uint64_t hi = s.value("ended_monotonic_ms",  (uint64_t)0);
                    if (hi > lo) ivs.push_back({lo, hi, s.value("content_hash",std::string())});
                }
                std::sort(ivs.begin(), ivs.end(),
                          [](const Iv& a, const Iv& b){ return a.lo < b.lo; });
                // Compare each interval against the running max-hi over ALL
                // earlier intervals (not just the predecessor), so a long
                // session containing several shorter ones can't be evaded
                // by interleaving start times.
                uint64_t max_hi = 0; std::string max_ch;
                for (size_t i = 0; i < ivs.size(); ++i) {
                    if (i > 0 && ivs[i].lo + 2000 < max_hi && ivs[i].ch != max_ch) {
                        hard_flag("ImplausibleSessionConcurrency"); break;
                    }
                    if (ivs[i].hi > max_hi) { max_hi = ivs[i].hi; max_ch = ivs[i].ch; }
                }
            }

            // (5) NoNetworkTransitions (SOFT): >=4 h span, zero transitions.
            if (bundle_span >= k4h && in.value("network_transitions", json::array()).empty())
                soft_flag("NoNetworkTransitions");

            // (6) ScreenAlwaysOn (SOFT, proxy signal — log only).
            {
                uint64_t on_ms = 0;
                for (const auto& iv : in.value("screen_intervals", json::array())) {
                    if (!iv.is_object()) continue;
                    uint64_t on = iv.value("on_wall_ms",(uint64_t)0);
                    uint64_t off= iv.value("off_wall_ms",(uint64_t)0);
                    if (off > on) on_ms += off - on;
                }
                if (bundle_span >= k4h) {
                    if (on_ms*100 >= bundle_span*98) soft_flag("ScreenAlwaysOn");
                    else if (on_ms == 0 && !h_sessions.empty()) soft_flag("ScreenAlwaysOn");
                }
            }

            // (7) BatteryFlatline (SOFT; skips on the -1 sentinel the client
            //     ships today — no false positives until real battery lands).
            {
                std::vector<int> pcts; bool any_charging = false;
                for (const auto& b : in.value("battery_samples", json::array())) {
                    if (!b.is_object()) continue;
                    int p = b.value("percent", -1);
                    if (b.value("charging", false)) any_charging = true;
                    if (p >= 0) pcts.push_back(p);
                }
                if (pcts.size() >= 4 && !any_charging && bundle_span >= 2*k1h) {
                    int lo = pcts[0], hi = pcts[0];
                    for (int p : pcts) { lo = std::min(lo,p); hi = std::max(hi,p); }
                    if (hi == lo) soft_flag("BatteryFlatline");
                }
            }

            // (8) StaticBSSIDLongSession (SOFT; skips on empty fingerprints
            //     the client ships today).
            {
                bool any_fp = false, any_handoff = false;
                for (const auto& t : in.value("network_transitions", json::array())) {
                    if (!t.is_object()) continue;
                    if (!t.value("fingerprint", std::string()).empty()) any_fp = true;
                    std::string k = t.value("kind", std::string());
                    if (k == "cell_handoff" || k == "wifi_roam") any_handoff = true;
                }
                if (any_fp && bundle_span >= k4h && !any_handoff)
                    soft_flag("StaticBSSIDLongSession");
            }

            // (9) DeviceIDChurn + (10) WalletAgeVsPlayVolume — need a rolling
            //     24 h per-address history persisted under obp:hist:<addr>.
            {
                const uint64_t now_ms = (uint64_t)std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                const uint64_t k24h = 24*k1h;
                json hist = json::array();
                if (auto raw = db_.get("obp:hist:" + addr_hex)) {
                    hist = json::parse(std::string(raw->begin(), raw->end()),
                                       nullptr, /*allow_exceptions=*/false);
                    if (!hist.is_array()) hist = json::array();
                }
                std::unordered_set<std::string> devs;
                uint64_t sessions_24h = 0;
                for (const auto& e : hist) {
                    if (!e.is_object()) continue;
                    uint64_t ts = e.value("ts",(uint64_t)0);
                    if (now_ms > ts && now_ms - ts > k24h) continue;  // prune >24 h
                    devs.insert(e.value("dev", std::string()));
                    sessions_24h += e.value("n",(uint64_t)0);
                }
                if (!device_id_hex.empty()) devs.insert(device_id_hex);
                sessions_24h += h_sessions.size();
                if (devs.size() > 3) hard_flag("DeviceIDChurn");
                const bool fresh_wallet = db_.get_nonce(player_addr) == 0
                                       && db_.get_balance(player_addr) == 0
                                       && hist.empty();
                if (fresh_wallet && sessions_24h > 200) hard_flag("WalletAgeVsPlayVolume");
            }

            if (bot_reject) {
                std::cout << "[rats-api] offline.play_proof.submit REJECTED (bot) from "
                          << addr_hex.substr(0,12) << " flags=" << flagged_patterns.size() << "\n";
                reply = {{"req_id", req_id}, {"status", "rejected"},
                         {"error", "bot heuristics flagged"},
                         {"flagged_patterns", flagged_patterns}};
                send_reply(peer_id, reply.dump());
                return;
            }

            // ---- 4. credit pipeline ---------------------------------
            // For each session in the bundle, replay
            // session.start → session.heartbeat[] → session.complete
            // through the existing HttpServer verbs so the union-of-
            // ranges math, 50% gate, MintTx call, and is_session_used
            // dedup are shared with the online path.
            //
            // The player's claimed session_id is recorded in the
            // response so the player can mark the matching log row as
            // submitted. The home node's authoritative session id is
            // whatever post_session_start hands back.
            json accepted_arr  = json::array();
            json rejected_arr  = json::array();
            const auto& sessions = in.value("sessions", json::array());
            for (const auto& s : sessions) {
                if (!s.is_object()) continue;
                const std::string claimed_sid = s.value("session_id", "");
                const std::string ch_hex      = s.value("content_hash", "");
                const auto& beats             = s.value("heartbeats",
                                                       json::array());
                // Open a server-side session for this content_hash.
                json start_body = {
                    {"content_hash",   ch_hex},
                    {"player_address", player_addr_hex},
                };
                auto [start_code, start_body_str] =
                    http_.verb_session_start(start_body.dump());
                if (start_code != 200) {
                    rejected_arr.push_back({
                        {"session_id", claimed_sid},
                        {"reason",     "session_start_failed"},
                        {"detail",     start_body_str},
                    });
                    continue;
                }
                json start_reply = json::parse(start_body_str,
                                                /*cb=*/nullptr,
                                                /*allow_exceptions=*/false);
                const std::string server_sid =
                    start_reply.value("session_id", std::string());
                if (server_sid.empty()) {
                    rejected_arr.push_back({
                        {"session_id", claimed_sid},
                        {"reason",     "session_id_missing"},
                    });
                    continue;
                }
                // Replay each heartbeat through the same verb the
                // online path uses. The server-side timestamps are
                // wall-clock-now per call — fine for the 50% gate
                // because we feed the beats in fast and the math is
                // relative to the session's own timeline.
                //
                // TODO(offline_play_proof): bypass the
                //   wall-clock-of-receipt model and inject each
                //   heartbeat's claimed wall_ms / monotonic_ms into
                //   PlaySession.samples directly. That preserves the
                //   actual playback cadence and lets the heuristics
                //   above operate on authentic gaps.
                bool any_beat_failed = false;
                for (const auto& b : beats) {
                    if (!b.is_object()) continue;
                    const uint64_t pos = b.value(
                        "position_ms", static_cast<uint64_t>(0));
                    json beat_body = {
                        {"session_id",  server_sid},
                        {"position_ms", pos},
                    };
                    auto [hb_code, hb_body_str] =
                        http_.verb_session_heartbeat(server_sid,
                                                      beat_body.dump());
                    if (hb_code != 200) {
                        any_beat_failed = true;
                        break;
                    }
                }
                if (any_beat_failed) {
                    rejected_arr.push_back({
                        {"session_id", claimed_sid},
                        {"reason",     "heartbeat_replay_failed"},
                    });
                    continue;
                }
                auto [cmp_code, cmp_body_str] =
                    http_.verb_session_complete(server_sid, "{}");
                if (cmp_code != 200) {
                    rejected_arr.push_back({
                        {"session_id",     claimed_sid},
                        {"reason",         "session_complete_failed"},
                        {"server_session", server_sid},
                        {"detail",         cmp_body_str},
                    });
                    continue;
                }
                json complete_reply = json::parse(cmp_body_str,
                                                   /*cb=*/nullptr,
                                                   /*allow_exceptions=*/false);
                accepted_arr.push_back({
                    {"session_id",        claimed_sid},
                    {"server_session_id", server_sid},
                    {"mint",              complete_reply},
                });
            }
            // Persist the replay marker now that the bundle is processed,
            // so a resubmit is rejected. Only mark when at least one
            // session credited, so a fully-failed bundle (e.g. transient
            // session_start failure) can be legitimately retried.
            if (!accepted_arr.empty()) {
                db_.put(replay_key, std::vector<uint8_t>{1});
                // Append this bundle to the per-address rolling history that
                // DeviceIDChurn / WalletAgeVsPlayVolume read (#5). Prune
                // entries older than 24 h so the record stays bounded.
                const uint64_t now_ms = (uint64_t)std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                const std::string hist_key = "obp:hist:" + addr_hex;
                json hist = json::array();
                if (auto raw = db_.get(hist_key)) {
                    hist = json::parse(std::string(raw->begin(), raw->end()),
                                       nullptr, false);
                    if (!hist.is_array()) hist = json::array();
                }
                json pruned = json::array();
                for (const auto& e : hist) {
                    if (!e.is_object()) continue;
                    uint64_t ts = e.value("ts",(uint64_t)0);
                    if (now_ms > ts && now_ms - ts > 24ULL*60*60*1000) continue;
                    pruned.push_back(e);
                }
                // Store SUBMITTED session count (h_sessions), matching what
                // the WalletAgeVsPlayVolume heuristic adds for the current
                // bundle — so historical and current counts use one metric.
                pruned.push_back({ {"ts", now_ms}, {"dev", device_id_hex},
                                   {"n", (uint64_t)h_sessions.size()} });
                const std::string blob = pruned.dump();
                db_.put(hist_key, std::vector<uint8_t>(blob.begin(), blob.end()));
            }
            reply = {{"req_id", req_id}, {"status", "ok"},
                     {"body", {
                         {"accepted",         accepted_arr},
                         {"rejected",         rejected_arr},
                         {"flagged_patterns", flagged_patterns},
                     }}};
            std::cout << "[rats-api] offline.play_proof.submit from "
                      << player_addr_hex.substr(0, 12) << ": accepted="
                      << accepted_arr.size() << " rejected="
                      << rejected_arr.size() << " flagged="
                      << flagged_patterns.size() << "\n";
        }
        // ---- unknown ----------------------------------------------------
        else {
            reply = {{"req_id", req_id},
                     {"status", "unknown_type"},
                     {"error",  "no handler for type=" + type}};
        }
    } catch (const std::exception& e) {
        reply = {{"req_id", req_id},
                 {"status", "server_error"},
                 {"error",  e.what()}};
    }

    // (#10) Relay crediting moved OUT of here. The old per-stream.open
    // `increment(mini, 1)` was unverifiable (the full node never sees a
    // relayed byte) and per-stream not per-byte. Credit now happens only
    // in try_corroborate(), when the broker's pending-delivery row, the
    // mini's signed relay.report, and the player's signed relay.receipt
    // all agree — per corroborated byte. See handle_relay_report/receipt.

    send_reply(peer_id, reply.dump());
}

// Thread-local reply sink for alternative transports — currently used
// by src/transport/ws_bridge.cpp to detour replies into a browser's
// WebSocket connection instead of librats. When non-null, send_reply
// hands the JSON to this closure and skips the rats path entirely.
// The bridge runs handle_request synchronously on the same thread that
// installed the sink, so a thread_local scope is correct: no two
// dispatches can race for the slot.
thread_local std::function<void(const std::string&)>* g_ws_reply_sink = nullptr;

void RatsApi::send_reply(const std::string& peer_id,
                         const std::string& reply_json) {
    if (g_ws_reply_sink && *g_ws_reply_sink) {
        (*g_ws_reply_sink)(reply_json);
        return;
    }
    if (!client_) return;
    rats_send_message(client_, peer_id.c_str(),
                      MC_REPLY_TYPE, reply_json.c_str());
}

// ---- Moderation gossip handlers -------------------------------------

void RatsApi::on_mod_action_cb(void* user_data, const char* peer_id,
                                const char* message_data) {
    auto* self = static_cast<RatsApi*>(user_data);
    if (!self || !peer_id || !message_data) {
        // Same ownership contract as on_request_cb: librats_c.cpp
        // strdup's both args before calling us, so we always free even
        // on the early-return path or both leak per inbound mod message.
        // Leaks of message_data add up fast on a healthy gossip mesh
        // because we receive a copy from every peer that re-broadcasts.
        if (peer_id)      rats_string_free(peer_id);
        if (message_data) rats_string_free(message_data);
        return;
    }
    try {
        self->handle_mod_envelope(peer_id, message_data,
                                   /*broadcast_if_new=*/true);
    } catch (const std::exception& e) {
        std::cerr << "[rats-api] mod handler threw: " << e.what() << "\n";
    }
    rats_string_free(peer_id);
    rats_string_free(message_data);
}

void RatsApi::handle_mod_envelope(const std::string& peer_id,
                                   const std::string& payload_json,
                                   bool broadcast_if_new) {
    moderation::Envelope env;
    try {
        auto j = nlohmann::json::parse(payload_json);
        if (!moderation::from_json(j, env)) return;
    } catch (const std::exception&) {
        return;
    }

    // Forgery reports (#4) ride the same envelope/log/replay machinery but
    // are node-signed (not moderator-gated) and handled with K-quorum +
    // re-audit instead of a direct db mutation. Branch before the
    // moderator-gated path below.
    if (env.action == "forgery_report") {
        handle_forgery_report(peer_id, payload_json, env, broadcast_if_new);
        return;
    }

    // Dedup BEFORE expensive verify so a re-broadcast storm bounces off
    // every node after first hop.
    if (db_.mod_log_has_sig(env.sig_hex)) return;

    if (!moderation::verify(env, db_)) {
        std::cerr << "[rats-api] mod envelope verify failed from "
                  << peer_id.substr(0, 12) << "\n";
        return;
    }

    leveldb::WriteBatch batch;
    if (!moderation::apply(env, db_, batch)) {
        std::cerr << "[rats-api] mod envelope unknown action="
                  << env.action << "\n";
        return;
    }
    db_.append_mod_log_entry(batch, env.ts_ms, env.sig_hex, payload_json);
    db_.write(batch);

    std::cout << "[rats-api] mod " << env.action << " value=\""
              << env.value.substr(0, 32) << "\" from "
              << peer_id.substr(0, 12)
              << " (replay-protected)\n";

    if (broadcast_if_new && client_) {
        // Re-broadcast to every other connected peer so a node that's
        // disconnected from the originator still learns about the
        // action by the second hop. Dedup at every receiver keeps this
        // from looping.
        rats_broadcast_string(client_, payload_json.c_str());
    }
}

// ======================================================================
// DB2 — wallet-signed library delta: canonical bytes, verify, ingest, flood
// ======================================================================
namespace {

void put_le(std::vector<uint8_t>& v, uint64_t x, int bytes) {
    for (int i = 0; i < bytes; ++i)
        v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}

// Canonical signed bytes for a library delta. The player's Dart signer MUST
// produce these byte-for-byte or the signature won't verify:
//   "mclib1"                     6-byte domain tag (versions the format)
//   wallet                       20
//   version                      8  LE
//   ts_ms                        8  LE
//   add_count                    4  LE
//   add_hashes                   add_count * 32   (in wire order)
//   del_count                    4  LE
//   del_hashes                   del_count * 32   (in wire order)
// The signer hashes this with SHA-256 and ECDSA-signs the digest; the wire
// order of add/del IS the signed order, so no canonical sort is needed.
std::vector<uint8_t> library_canonical(const Address& wallet,
                                       uint64_t version, uint64_t ts_ms,
                                       const std::vector<Hash256>& add,
                                       const std::vector<Hash256>& del) {
    std::vector<uint8_t> b;
    static const char tag[6] = {'m', 'c', 'l', 'i', 'b', '1'};
    b.insert(b.end(), tag, tag + 6);
    b.insert(b.end(), wallet.begin(), wallet.end());
    put_le(b, version, 8);
    put_le(b, ts_ms,   8);
    put_le(b, add.size(), 4);
    for (const auto& h : add) b.insert(b.end(), h.begin(), h.end());
    put_le(b, del.size(), 4);
    for (const auto& h : del) b.insert(b.end(), h.begin(), h.end());
    return b;
}

} // namespace

bool RatsApi::ingest_library_delta(const std::string& payload_json,
                                   bool broadcast_if_new) {
    json env;
    try { env = json::parse(payload_json); } catch (...) { return false; }
    if (!env.is_object()) return false;

    Address  wallet{};
    PubKey33 pubkey{};
    Sig64    sig{};
    if (!crypto::parse_address_checksummed(env.value("wallet", std::string()), wallet))
        return false;
    if (!from_hex_fixed(env.value("pubkey", std::string()), pubkey.data(), pubkey.size()))
        return false;
    if (!from_hex_fixed(env.value("sig", std::string()), sig.data(), sig.size()))
        return false;
    // Bind the signature to the wallet: the pubkey must derive to it.
    if (crypto::address_from_pubkey(pubkey) != wallet) return false;

    const uint64_t version = env.value("version", static_cast<uint64_t>(0));
    const uint64_t ts_ms   = env.value("ts",      static_cast<uint64_t>(0));
    std::vector<Hash256> add, del;
    if (env.contains("add") && env["add"].is_array())
        for (const auto& h : env["add"]) {
            Hash256 ch{};
            if (h.is_string() && crypto::parse_hash256(h.get<std::string>(), ch))
                add.push_back(ch);
        }
    if (env.contains("del") && env["del"].is_array())
        for (const auto& h : env["del"]) {
            Hash256 ch{};
            if (h.is_string() && crypto::parse_hash256(h.get<std::string>(), ch))
                del.push_back(ch);
        }

    // Verify the wallet's signature over the canonical bytes.
    const auto    canon  = library_canonical(wallet, version, ts_ms, add, del);
    const Hash256 digest = crypto::sha256(canon.data(), canon.size());
    if (!crypto::verify_ecdsa(digest, sig, pubkey)) return false;

    // Apply as a version-gated SNAPSHOT: the published `add` set is the full,
    // authoritative library, so removals propagate (a node that had extra
    // songs drops them). `del` is still covered by the signature above. Store
    // the signed payload so a node can re-send it during anti-entropy.
    if (!library_.set_library(wallet, add, version)) return false;
    library_.store_library_payload(wallet, version, payload_json);

    if (broadcast_if_new && client_)
        rats_broadcast_message(client_, MC_LIBRARY_TYPE, payload_json.c_str());
    return true;
}

void RatsApi::on_library_cb(void* user_data, const char* /*peer_id*/,
                            const char* message_data) {
    auto* self = static_cast<RatsApi*>(user_data);
    if (!self || !message_data) return;
    self->ingest_library_delta(message_data, /*broadcast_if_new=*/true);
}

// ---- DB2 playlists: signed record, verify, ingest, flood ----------------
namespace {
// Canonical signed bytes for a playlist record (must match the player's Dart):
//   "mcpls1" || wallet(20) || playlist_id(16) || version(8 LE) || ts(8 LE) ||
//   deleted(1) || name_len(2 LE) || name || count(4 LE) || song_hashes(32·n)
std::vector<uint8_t> playlist_canonical(const Address& wallet,
                                        const std::array<uint8_t, 16>& pid,
                                        uint64_t version, uint64_t ts_ms,
                                        bool deleted, const std::string& name,
                                        const std::vector<Hash256>& songs) {
    std::vector<uint8_t> b;
    static const char tag[6] = {'m', 'c', 'p', 'l', 's', '1'};
    b.insert(b.end(), tag, tag + 6);
    b.insert(b.end(), wallet.begin(), wallet.end());
    b.insert(b.end(), pid.begin(), pid.end());
    put_le(b, version, 8);
    put_le(b, ts_ms,   8);
    b.push_back(deleted ? 1 : 0);
    put_le(b, name.size(), 2);
    b.insert(b.end(), name.begin(), name.end());
    put_le(b, songs.size(), 4);
    for (const auto& h : songs) b.insert(b.end(), h.begin(), h.end());
    return b;
}
} // namespace

bool RatsApi::ingest_playlist(const std::string& payload_json,
                              bool broadcast_if_new) {
    json env;
    try { env = json::parse(payload_json); } catch (...) { return false; }
    if (!env.is_object()) return false;

    Address                 wallet{};
    PubKey33                pubkey{};
    Sig64                   sig{};
    std::array<uint8_t, 16> pid{};
    if (!crypto::parse_address_checksummed(env.value("wallet", std::string()), wallet))
        return false;
    if (!from_hex_fixed(env.value("pubkey", std::string()), pubkey.data(), pubkey.size()))
        return false;
    if (!from_hex_fixed(env.value("sig", std::string()), sig.data(), sig.size()))
        return false;
    if (!from_hex_fixed(env.value("playlist_id", std::string()), pid.data(), pid.size()))
        return false;
    if (crypto::address_from_pubkey(pubkey) != wallet) return false;

    const uint64_t    version = env.value("version", static_cast<uint64_t>(0));
    const uint64_t    ts_ms   = env.value("ts",      static_cast<uint64_t>(0));
    const bool        deleted = env.value("deleted", false);
    const std::string name    = env.value("name",    std::string());
    std::vector<Hash256> songs;
    if (env.contains("songs") && env["songs"].is_array())
        for (const auto& h : env["songs"]) {
            Hash256 ch{};
            if (h.is_string() && crypto::parse_hash256(h.get<std::string>(), ch))
                songs.push_back(ch);
        }

    const auto    canon  = playlist_canonical(wallet, pid, version, ts_ms,
                                              deleted, name, songs);
    const Hash256 digest = crypto::sha256(canon.data(), canon.size());
    if (!crypto::verify_ecdsa(digest, sig, pubkey)) return false;

    const bool applied = deleted
        ? library_.delete_playlist(wallet, pid, version)
        : library_.set_playlist(wallet, pid, name, songs, version);
    if (!applied) return false;
    library_.store_playlist_payload(wallet, pid, version, payload_json);

    if (broadcast_if_new && client_)
        rats_broadcast_message(client_, MC_PLAYLIST_TYPE, payload_json.c_str());
    return true;
}

void RatsApi::on_playlist_cb(void* user_data, const char* /*peer_id*/,
                             const char* message_data) {
    auto* self = static_cast<RatsApi*>(user_data);
    if (!self || !message_data) return;
    self->ingest_playlist(message_data, /*broadcast_if_new=*/true);
}

bool RatsApi::publish_mod_action(const std::string&         action,
                                  const std::string&         value,
                                  const mc::crypto::KeyPair& moderator_kp) {
    moderation::Envelope env = moderation::sign(action, value, moderator_kp);
    const std::string payload = moderation::to_json(env).dump();

    leveldb::WriteBatch batch;
    if (!moderation::apply(env, db_, batch)) return false;
    db_.append_mod_log_entry(batch, env.ts_ms, env.sig_hex, payload);
    db_.write(batch);

    if (client_) rats_broadcast_string(client_, payload.c_str());
    return true;
}

// ---- Forgery reports (#4) -------------------------------------------

void RatsApi::publish_forgery_report(const Hash256& content_hash,
                                     const Hash256& block_hash,
                                     float sim, const Hash256& audio_sha) {
    nlohmann::json v = {
        {"ch",  crypto::to_hex(content_hash)},
        {"bh",  crypto::to_hex(block_hash)},
        {"sim", sim},
        {"afp", crypto::to_hex(audio_sha)},
    };
    // Node-signed (keypair_), not moderator-signed — a machine attestation.
    moderation::Envelope env =
        moderation::sign("forgery_report", v.dump(), keypair_);
    const std::string payload = moderation::to_json(env).dump();
    // Route through the same handler live reports take: records our report
    // into the fr: tally, appends to the mod-log (so it replays to late
    // joiners), and broadcasts. The auditor already marked the song deleted
    // locally before calling us.
    handle_forgery_report(/*peer_id=*/"self", payload, env,
                          /*broadcast_if_new=*/true);
}

void RatsApi::handle_forgery_report(const std::string& peer_id,
                                     const std::string& payload_json,
                                     const moderation::Envelope& env,
                                     bool broadcast_if_new) {
    constexpr int kForgeryQuorum = 2;   // distinct independent reporters

    // (a) crypto + structural gate.
    if (db_.mod_log_has_sig(env.sig_hex)) return;          // replay-storm dedup
    if (!moderation::verify_signature_only(env)) {
        std::cerr << "[forgery] bad sig from " << peer_id.substr(0, 12) << "\n";
        return;
    }
    Hash256 ch{}, bh{};
    float sim = 1.0f;
    try {
        auto v = nlohmann::json::parse(env.value);
        if (!crypto::parse_hash256(v.value("ch", std::string()), ch)) return;
        crypto::parse_hash256(v.value("bh", std::string()), bh);
        sim = v.value("sim", 1.0f);
    } catch (...) { return; }
    if (sim >= DeepAuditor::kSlashThreshold) return;       // not a forgery claim
    // bh must resolve to a real on-chain block actually carrying ch — this
    // rejects fabricated reports against songs that were never registered.
    {
        auto blk = chain_.get_block(bh);
        if (!blk || !blk->has_song || blk->song.content_hash != ch) {
            std::cerr << "[forgery] report names a block not carrying ch — drop\n";
            return;
        }
    }
    auto pub_bytes = crypto::from_hex(env.mod_pub_hex);
    if (pub_bytes.size() != 33) return;
    PubKey33 pub{};
    std::copy(pub_bytes.begin(), pub_bytes.end(), pub.begin());
    const Address reporter = crypto::address_from_pubkey(pub);

    leveldb::WriteBatch batch;
    // Record (idempotent per (ch, reporter)) + append to mod-log so the
    // report replays to late joiners via mod.sync_since.
    const std::string fr_key =
        "fr:" + crypto::to_hex(ch) + ":" + crypto::to_hex(reporter.data(), 20);
    const bool reporter_new = !db_.get(fr_key).has_value();
    db_.record_forgery_report(batch, ch, reporter);
    db_.append_mod_log_entry(batch, env.ts_ms, env.sig_hex, payload_json);

    // Decide whether to invalidate: (c) re-audit if we hold the bytes,
    // else (b) K-independent-reports quorum.
    bool act = false;
    auto local = audit_content(db_, audio_dir_, ch);
    if (local) {
        if (!local->ok) act = true;                        // we confirmed forgery
        else std::cerr << "[forgery] re-audit says audio MATCHES — recording "
                          "report but NOT deleting\n";
    } else {
        const int effective =
            db_.forgery_report_count(ch) + (reporter_new ? 1 : 0);
        if (effective >= kForgeryQuorum) act = true;
    }
    if (act && !db_.is_song_deleted(ch)) {
        db_.mark_song_deleted(batch, ch);
        std::cout << "[forgery] content " << crypto::to_hex(ch).substr(0, 16)
                  << "… marked deleted (quorum/re-audit)\n";
    }
    db_.write(batch);

    if (broadcast_if_new && client_)
        rats_broadcast_string(client_, payload_json.c_str());
}

// ---- #10 relay-reward triangulation (broker side) -------------------
//
// pd:<delivery_id hex> row (JSON): { ch, broker, created, mw, relayed,
// received, flags }. flags bit0=brokered, bit1=reported, bit2=receipted.
// Credit fires only when all three are set, per min(relayed,received) byte.

std::string RatsApi::mint_delivery(const Hash256& content_hash) {
    uint8_t did[16];
    if (RAND_bytes(did, 16) != 1) {
        // Fallback: hash (now_ms || content_hash) — still unguessable enough.
        const uint64_t t = (uint64_t)std::chrono::duration_cast<
            std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        std::vector<uint8_t> seed(content_hash.begin(), content_hash.end());
        for (int i=0;i<8;++i) seed.push_back((uint8_t)(t >> (8*i)));
        Hash256 h = crypto::sha256(seed.data(), seed.size());
        std::copy(h.begin(), h.begin()+16, did);
    }
    const std::string did_hex = crypto::to_hex(did, 16);
    const uint64_t now = (uint64_t)std::chrono::duration_cast<
        std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    nlohmann::json row = {
        {"ch",       crypto::to_hex(content_hash)},
        {"broker",   crypto::to_hex(keypair_.address.data(), 20)},
        {"created",  now},
        {"mw",       std::string()},
        {"relayed",  (uint64_t)0},
        {"received", (uint64_t)0},
        {"flags",    1},
    };
    const std::string s = row.dump();
    db_.put("pd:" + did_hex, std::vector<uint8_t>(s.begin(), s.end()));
    return did_hex;
}

bool RatsApi::handle_relay_report(const nlohmann::json& body) {
    const std::string did_hex = body.value("delivery_id", std::string());
    const uint64_t bytes_relayed = body.value("bytes_relayed", (uint64_t)0);
    const std::string mw_hex  = body.value("mini_wallet",  std::string());
    const std::string pk_hex  = body.value("mini_pubkey",  std::string());
    const std::string sig_hex = body.value("sig",          std::string());
    if (did_hex.size() != 32) return false;
    auto row_opt = db_.get("pd:" + did_hex);
    if (!row_opt) return false;                 // we didn't broker it — drop
    auto pk = crypto::from_hex(pk_hex); if (pk.size() != 33) return false;
    PubKey33 pub{}; std::copy(pk.begin(), pk.end(), pub.begin());
    Address mini_addr{};
    if (!crypto::parse_address_checksummed(mw_hex, mini_addr)) {
        auto a = crypto::from_hex(mw_hex);
        if (a.size() != 20) return false;
        std::copy(a.begin(), a.end(), mini_addr.begin());
    }
    if (crypto::address_from_pubkey(pub) != mini_addr) return false;
    auto sigb = crypto::from_hex(sig_hex); if (sigb.size() != 64) return false;
    Sig64 sig{}; std::copy(sigb.begin(), sigb.end(), sig.begin());
    auto did = crypto::from_hex(did_hex); if (did.size() != 16) return false;
    // preimage = "relay.report" || did(16) || bytes(u64 LE) || mini_addr(20)
    std::vector<uint8_t> msg;
    const char* tag = "relay.report";
    msg.insert(msg.end(), tag, tag + std::strlen(tag));
    msg.insert(msg.end(), did.begin(), did.end());
    for (int i=0;i<8;++i) msg.push_back((uint8_t)(bytes_relayed >> (8*i)));
    msg.insert(msg.end(), mini_addr.begin(), mini_addr.end());
    if (!crypto::verify_data(msg.data(), msg.size(), sig, pub)) return false;
    auto row = nlohmann::json::parse(
        std::string(row_opt->begin(), row_opt->end()), nullptr, false);
    if (!row.is_object()) return false;
    row["mw"]      = crypto::to_hex(mini_addr.data(), 20);
    row["relayed"] = bytes_relayed;
    row["flags"]   = row.value("flags", 0) | 2;
    const std::string s = row.dump();
    db_.put("pd:" + did_hex, std::vector<uint8_t>(s.begin(), s.end()));
    try_corroborate(did_hex);
    return true;
}

bool RatsApi::handle_relay_receipt(const nlohmann::json& body) {
    const std::string did_hex = body.value("delivery_id", std::string());
    const std::string ch_hex  = body.value("content_hash", std::string());
    const uint64_t bytes_recv = body.value("bytes_received", (uint64_t)0);
    const std::string pa_hex  = body.value("player_address", std::string());
    const std::string pk_hex  = body.value("player_pubkey",  std::string());
    const std::string sig_hex = body.value("sig",            std::string());
    if (did_hex.size() != 32 || ch_hex.size() != 64) return false;
    auto row_opt = db_.get("pd:" + did_hex);
    if (!row_opt) return false;
    auto row = nlohmann::json::parse(
        std::string(row_opt->begin(), row_opt->end()), nullptr, false);
    if (!row.is_object()) return false;
    if (row.value("ch", std::string()) != ch_hex) return false;  // wrong song
    auto pk = crypto::from_hex(pk_hex); if (pk.size() != 33) return false;
    PubKey33 pub{}; std::copy(pk.begin(), pk.end(), pub.begin());
    Address player{};
    if (!crypto::parse_address_checksummed(pa_hex, player)) return false;
    if (crypto::address_from_pubkey(pub) != player) return false;
    auto sigb = crypto::from_hex(sig_hex); if (sigb.size() != 64) return false;
    Sig64 sig{}; std::copy(sigb.begin(), sigb.end(), sig.begin());
    auto did = crypto::from_hex(did_hex); if (did.size() != 16) return false;
    auto ch  = crypto::from_hex(ch_hex);  if (ch.size()  != 32) return false;
    // preimage = "relay.receipt" || did(16) || content_hash(32) || bytes(u64 LE)
    std::vector<uint8_t> msg;
    const char* tag = "relay.receipt";
    msg.insert(msg.end(), tag, tag + std::strlen(tag));
    msg.insert(msg.end(), did.begin(), did.end());
    msg.insert(msg.end(), ch.begin(),  ch.end());
    for (int i=0;i<8;++i) msg.push_back((uint8_t)(bytes_recv >> (8*i)));
    if (!crypto::verify_data(msg.data(), msg.size(), sig, pub)) return false;
    row["received"] = bytes_recv;
    row["flags"]    = row.value("flags", 0) | 4;
    const std::string s = row.dump();
    db_.put("pd:" + did_hex, std::vector<uint8_t>(s.begin(), s.end()));
    try_corroborate(did_hex);
    return true;
}

void RatsApi::try_corroborate(const std::string& did_hex) {
    auto row_opt = db_.get("pd:" + did_hex);
    if (!row_opt) return;
    auto row = nlohmann::json::parse(
        std::string(row_opt->begin(), row_opt->end()), nullptr, false);
    if (!row.is_object()) return;
    if ((row.value("flags", 0) & 7) != 7) return;   // brokered+reported+receipted
    const uint64_t relayed  = row.value("relayed",  (uint64_t)0);
    const uint64_t received = row.value("received", (uint64_t)0);
    // 1 MC (1e8 internal units) per 10 MB ⇒ 10 internal units per byte.
    constexpr uint64_t kUnitsPerByte = 10;
    // Clamp BEFORE the multiply so a colluding mini+player can't report
    // absurd byte counts that overflow u64 and wrap to a small/garbage
    // credit (or worse). The per-tx caps downstream only see the product,
    // so the guard has to live here. UINT64_MAX/10 bytes ≈ 1.8e18 — far
    // above any real delivery, so honest traffic is never touched.
    const uint64_t credited =
        std::min<uint64_t>(std::min(relayed, received), UINT64_MAX / kUnitsPerByte);
    auto a = crypto::from_hex(row.value("mw", std::string()));
    if (a.size() == 20 && relay_tracker_ && credited > 0) {
        Address mini{}; std::copy(a.begin(), a.end(), mini.begin());
        relay_tracker_->increment(mini, credited * kUnitsPerByte);
        std::cout << "[relay] corroborated delivery " << did_hex.substr(0, 12)
                  << "… credited " << credited << "B → "
                  << credited * kUnitsPerByte << " units\n";
    }
    db_.del("pd:" + did_hex);   // single-use ⇒ replay-proof
}

void RatsApi::record_attestation_level(const std::string& device_id,
                                       const std::string& level) {
    if (device_id.empty()) return;
    const uint64_t now = (uint64_t)std::chrono::duration_cast<
        std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string rec = level + "|" + std::to_string(now);
    db_.put("dal:" + device_id, std::vector<uint8_t>(rec.begin(), rec.end()));
}

void RatsApi::push_mod_log_since(const std::string& peer_id,
                                  uint64_t            since_ts_ms) {
    if (!client_) return;
    size_t pushed = 0;
    db_.iter_mod_log_since(since_ts_ms,
        [&](uint64_t /*ts*/, const std::string& /*sig*/, const std::string& payload){
            rats_send_message(client_, peer_id.c_str(),
                              MC_MOD_TYPE, payload.c_str());
            ++pushed;
            return true;
        });
    if (pushed > 0) {
        std::cout << "[rats-api] mod.sync_since pushed " << pushed
                  << " envelope(s) to " << peer_id.substr(0, 12) << "\n";
    }
}

} // namespace mc::api
