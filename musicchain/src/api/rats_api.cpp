#include "rats_api.h"
#include "server.h"
#include "../audio/fingerprint.h"
#include "../moderation/mod_action.h"
#include "../util/traffic.h"

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
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>

using json = nlohmann::json;

namespace mc::api {

constexpr const char* MC_REQUEST_TYPE = "musicchain.request";
constexpr const char* MC_REPLY_TYPE   = "musicchain.reply";
constexpr const char* MC_MOD_TYPE     = "musicchain.mod";

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
}

void RatsApi::start(rats_client_t client) {
    if (client_) return;
    client_ = client;
    rats_on_message(client_, MC_REQUEST_TYPE,
                    &RatsApi::on_request_cb, this);
    rats_on_message(client_, MC_MOD_TYPE,
                    &RatsApi::on_mod_action_cb, this);
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
        }
    }
    if (peer_id) rats_string_free(peer_id);
}

void RatsApi::on_peer_disconnected_cb(void* user_data, const char* peer_id) {
    auto* self = static_cast<RatsApi*>(user_data);
    if (self && peer_id && *peer_id) {
        self->swarm_.mark_peer_offline(peer_id);
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
                 const net::NodeConfig& config) {
    return {
        {"role",         "full-node"},
        {"node_id",      mc::crypto::to_hex(config.node_id)},
        {"chain_height", chain.tip().height},
        {"peer_count",   network.peer_count()},
        {"own_ipv6",     network.own_ipv6_str()},
        {"api_port",     config.api_port},
        {"p2p_port",     config.p2p_port},
    };
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
                     {"body",   status_body(chain_, network_, config_)}};
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
        // ---- chain sync ---------------------------------------------
        //
        // chain.tip               → { height, hash, timestamp_ms }
        // chain.list_block_hashes → { from_height, max=128 } →
        //                            { hashes: [hex,hex,...] }
        // chain.get_block         → { hash } | { height } →
        //                            { block_b64: "..." }
        //
        // SyncManager on a fresh node calls chain.tip on every peer it
        // can reach, picks the best peer per fork-choice, then walks
        // chain.list_block_hashes paginated forward from its current
        // tip+1 and pulls each block via chain.get_block. Each pulled
        // block runs the same five-step validation rebuild_derived_state
        // runs before connect_block.
        else if (type == "chain.tip") {
            const auto t = chain_.tip();
            reply = {{"req_id", req_id}, {"status", "ok"},
                     {"body", {{"height",       t.height},
                                {"hash",         crypto::to_hex(t.hash)},
                                {"timestamp_ms", t.timestamp_ms}}}};
        } else if (type == "chain.list_block_hashes") {
            const uint32_t from = static_cast<uint32_t>(
                in.value("from_height", 0));
            uint32_t max  = static_cast<uint32_t>(in.value("max", 128));
            if (max == 0 || max > 512) max = 128; // cap per-call cost
            const auto t = chain_.tip();
            std::vector<std::string> out;
            for (uint32_t h = from; h <= t.height && out.size() < max; ++h) {
                if (auto bh = chain_.get_block_hash(h))
                    out.push_back(crypto::to_hex(*bh));
                else break;
            }
            reply = {{"req_id", req_id}, {"status", "ok"},
                     {"body", {{"from_height", from},
                                {"hashes",      out}}}};
        } else if (type == "chain.get_block") {
            std::optional<Block> block;
            if (in.contains("hash")) {
                Hash256 h{};
                if (crypto::parse_hash256(in.value("hash", ""), h))
                    block = chain_.get_block(h);
            } else if (in.contains("height")) {
                const uint32_t height =
                    static_cast<uint32_t>(in.value("height", 0));
                if (auto bh = chain_.get_block_hash(height))
                    block = chain_.get_block(*bh);
            }
            if (!block) {
                reply = {{"req_id", req_id}, {"status", "not_found"},
                         {"body", nullptr}};
            } else {
                const auto bytes = block->serialize();
                reply = {{"req_id", req_id}, {"status", "ok"},
                         {"body", {{"block_b64",
                                    audio::base64_encode(bytes.data(),
                                                          bytes.size())}}}};
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
        // ---- session control --------------------------------------------
        else if (type == "session.start") {
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
                auto members = swarm_.members(ch);
                if (members.empty()) {
                    reply = {{"req_id", req_id},
                             {"status", "ok"},
                             {"body",   {
                                 {"content_hash", hash},
                                 {"peers",        nlohmann::json::array()},
                                 {"source",       "no_swarm"},
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
                             }}};
                }
            }
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
                    constexpr float kSimThreshold = 0.55f;
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
                            }
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
                        const std::string aa_hex     = in.value("artist_address",
                                                                std::string());
                        if (aa_hex.size() == 40) {
                            auto raw = crypto::from_hex(aa_hex);
                            if (raw.size() == 20) {
                                std::copy(raw.begin(), raw.end(),
                                          reg.artist_address.begin());
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
            if (!crypto::parse_address(player_addr_hex, player_addr)) {
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

            // ---- 2. replay protection --------------------------------
            // TODO(offline_play_proof): persist used (player_address,
            //   bundle_nonce) pairs to leveldb so cross-restart replay
            //   is rejected. For now keep an in-process set so at least
            //   same-uptime replays bounce.
            (void)nonce_hex;
            (void)created_at_ms;

            // ---- 3. bot-detection heuristics (stubbed) ---------------
            // Each heuristic returns true when the bundle looks
            // plausibly real; false adds the rule name to
            // `flagged_patterns` and (once thresholds land) trips the
            // bundle into "rejected" status. Currently all return true
            // so the credit pipeline is exercisable end-to-end.
            std::vector<std::string> flagged_patterns;
            auto heuristic = [&](const char* name, bool ok) {
                if (!ok) flagged_patterns.emplace_back(name);
            };

            // TODO(offline_play_proof): PerfectIntervalHeartbeats —
            //   σ of inter-heartbeat gaps < 50 ms over 30+ beats means
            //   the bot used a tight loop instead of real playback.
            heuristic("PerfectIntervalHeartbeats", /*plausible=*/true);

            // TODO(offline_play_proof): MonotonicClockJumps — within
            //   each session, monotonic_ms must be non-decreasing and
            //   correlate with wall_ms (±2 s slop). Reject any session
            //   where the delta lies outside that envelope.
            heuristic("MonotonicClockJumps", true);

            // TODO(offline_play_proof): StaticBSSIDLongSession —
            //   4 h+ "cellular" span with a single cell_id is a strong
            //   forgery signal. Real cell handoff happens every 10-30
            //   min when stationary, every 1-2 min in transit.
            heuristic("StaticBSSIDLongSession", true);

            // TODO(offline_play_proof): BatteryFlatline — battery_%
            //   slope of zero across hours of active playback is
            //   physically implausible for a phone playing audio.
            heuristic("BatteryFlatline", true);

            // TODO(offline_play_proof): ScreenAlwaysOn — screen on for
            //   the full bundle window is suspicious; so is screen
            //   never on. Real listeners lock and unlock.
            heuristic("ScreenAlwaysOn", true);

            // TODO(offline_play_proof): NoNetworkTransitions — long
            //   offline span (hours) with zero transitions logged is
            //   incompatible with a real device's radio behaviour.
            heuristic("NoNetworkTransitions", true);

            // TODO(offline_play_proof): HeartbeatDensityTooHigh — > 12
            //   heartbeats per song-second is over-densification meant
            //   to game the union-of-ranges integration.
            heuristic("HeartbeatDensityTooHigh", true);

            // TODO(offline_play_proof): ImplausibleSessionConcurrency —
            //   overlapping sessions for different content_hashes on
            //   the same device. A phone plays one song at a time.
            heuristic("ImplausibleSessionConcurrency", true);

            // TODO(offline_play_proof): DeviceIDChurn — same
            //   player_address bundling many distinct device_ids per
            //   day. Real users have 1-3 devices.
            heuristic("DeviceIDChurn", true);

            // TODO(offline_play_proof): WalletAgeVsPlayVolume — fresh
            //   wallet (no on-chain history) submitting hundreds of
            //   plays/day. Common farming signature.
            heuristic("WalletAgeVsPlayVolume", true);

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

    send_reply(peer_id, reply.dump());
}

void RatsApi::send_reply(const std::string& peer_id,
                         const std::string& reply_json) {
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
