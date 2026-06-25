#include "server.h"
#include "routes.h"
#include "../crypto/hash.h"
#include "../crypto/keys.h"
#include "../crypto/signature.h"
#include "../tokens/ledger.h"
#include "../tokens/mint.h"
#include "../core/transaction.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <random>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

using json = nlohmann::json;

namespace mc::api {

// ---- Multipart parser -----------------------------------------------

struct MultipartPart {
    std::string          name;
    std::string          filename;
    std::vector<uint8_t> data;
};

static std::vector<MultipartPart> parse_multipart(const std::string& body,
                                                    const std::string& boundary) {
    std::vector<MultipartPart> parts;
    std::string delim     = "\r\n--" + boundary;
    std::string first_del = "--"     + boundary;

    size_t pos = body.find(first_del);
    if (pos == std::string::npos) return parts;
    pos += first_del.size();
    if (pos + 1 < body.size() && body[pos] == '\r') pos += 2;
    else return parts;

    while (pos < body.size()) {
        size_t next = body.find(delim, pos);
        if (next == std::string::npos) break;

        std::string part_str = body.substr(pos, next - pos);
        size_t hend = part_str.find("\r\n\r\n");
        if (hend == std::string::npos) break;

        MultipartPart part;
        std::string hdrs = part_str.substr(0, hend);
        size_t h = 0;
        while (h < hdrs.size()) {
            size_t nl = hdrs.find("\r\n", h);
            if (nl == std::string::npos) nl = hdrs.size();
            std::string line  = hdrs.substr(h, nl - h);
            std::string lline = line;
            for (auto& c : lline)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lline.find("content-disposition") == 0) {
                auto np = line.find("name=\"");
                if (np != std::string::npos) {
                    np += 6;
                    auto ne = line.find('"', np);
                    if (ne != std::string::npos) part.name = line.substr(np, ne - np);
                }
                auto fp = line.find("filename=\"");
                if (fp != std::string::npos) {
                    fp += 10;
                    auto fe = line.find('"', fp);
                    if (fe != std::string::npos) part.filename = line.substr(fp, fe - fp);
                }
            }
            h = (nl >= hdrs.size()) ? nl : nl + 2;
        }
        size_t body_start = hend + 4;
        part.data.assign(part_str.begin() + body_start, part_str.end());
        parts.push_back(std::move(part));

        pos = next + delim.size();
        if (pos + 1 < body.size() && body[pos] == '-') break; // terminal "--"
        if (pos + 1 < body.size() && body[pos] == '\r') pos += 2;
        else break;
    }
    return parts;
}

static std::string extract_boundary(const std::string& content_type) {
    auto p = content_type.find("boundary=");
    if (p == std::string::npos) return {};
    std::string b = content_type.substr(p + 9);
    // Strip optional quotes
    if (!b.empty() && b[0] == '"') b = b.substr(1);
    auto q = b.find('"');
    if (q != std::string::npos) b = b.substr(0, q);
    // Strip trailing whitespace/semicolons
    while (!b.empty() && (b.back() == ' ' || b.back() == ';')) b.pop_back();
    return b;
}

static uint64_t now_ms_api() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// ---- Constructor / Destructor ---------------------------------------

HttpServer::HttpServer(Chain& chain, CandidateManager& candidates,
                       net::NetworkManager& network, Database& db,
                       const net::NodeConfig& config,
                       const mc::crypto::KeyPair& keypair)
    : chain_(chain), candidates_(candidates), network_(network), db_(db),
      config_(config), node_keypair_(keypair) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start() {
    // microhttpd HTTP/1.1 listener removed in Phase 2c. The class is kept
    // because both RatsApi (rats RPC) and H3Server (HTTP/3) dispatch to its
    // verb_* methods. No socket is opened here.
    return true;
}

void HttpServer::stop() {}

// MHD access_handler + handle_request removed in Phase 2c — HTTP routing
// now lives in transport/h3_server.cpp on top of msh3. The verb methods
// below stay as the canonical implementation, called by both the HTTP/3
// dispatcher and `rats_api.cpp` over QUIC.
#if 0  // ---- legacy MHD path, kept as comment for reference ----
auto _legacy_handle_request_signature_only(void* conn,
                                       const std::string& url,
                                       const std::string& method,
                                       const std::string& body) {
    auto segs = parse_path(url);

    // GET /status
    if (method == "GET" && segs.size() == 1 && segs[0] == "status")
        return send_json(conn, 200, get_status().second);

    // GET /peers
    if (method == "GET" && segs.size() == 1 && segs[0] == "peers")
        return send_json(conn, 200, get_peers().second);

    // /net/dht-peers and /upload removed — both now flow over librats.
    (void)body;

    // GET /blocks/{hash}
    if (method == "GET" && segs.size() == 2 && segs[0] == "blocks")
        return send_json(conn, 200, get_block(segs[1]).second);

    // GET /blocks/height/{height}
    if (method == "GET" && segs.size() == 3 && segs[0] == "blocks" && segs[1] == "height") {
        uint32_t h = static_cast<uint32_t>(std::stoul(segs[2]));
        return send_json(conn, 200, get_block_at_height(h).second);
    }

    // GET /songs/search?artist=X&genre=Y&q=Z  (must come before /songs/{hash})
    if (method == "GET" && segs.size() == 2 && segs[0] == "songs" && segs[1] == "search")
        return send_json(conn, 200, get_songs_search(conn).second);

    // GET /songs
    if (method == "GET" && segs.size() == 1 && segs[0] == "songs")
        return send_json(conn, 200, get_songs_list().second);

    // GET /songs/{content_hash}
    if (method == "GET" && segs.size() == 2 && segs[0] == "songs")
        return send_json(conn, 200, get_song(segs[1]).second);

    // /songs/{hash}/stream removed: the full node never holds audio bytes
    // under the post-pivot architecture. Clients fetch by hitting a swarm
    // peer (see fingerprint.submit + stream.open swarm reply).

    // GET /balances/{address}
    if (method == "GET" && segs.size() == 2 && segs[0] == "balances")
        return send_json(conn, 200, get_balance(segs[1]).second);

    // POST /sessions/start
    if (method == "POST" && segs.size() == 2 && segs[0] == "sessions" && segs[1] == "start")
        return send_json(conn, 200, post_session_start(body).second);

    // POST /sessions/{id}/heartbeat
    if (method == "POST" && segs.size() == 3 && segs[0] == "sessions" && segs[2] == "heartbeat")
        return send_json(conn, 200, post_session_heartbeat(segs[1], body).second);

    // POST /sessions/{id}/complete
    if (method == "POST" && segs.size() == 3 && segs[0] == "sessions" && segs[2] == "complete")
        return send_json(conn, 200, post_session_complete(segs[1], body).second);

    // GET /wallet/address
    if (method == "GET" && segs.size() == 2 && segs[0] == "wallet" && segs[1] == "address")
        return send_json(conn, 200, get_wallet_address().second);

    // GET /wallet/{address}/nonce
    if (method == "GET" && segs.size() == 3 && segs[0] == "wallet" && segs[2] == "nonce")
        return send_json(conn, 200, get_wallet_nonce(segs[1]).second);

    // POST /wallet/create
    if (method == "POST" && segs.size() == 2 && segs[0] == "wallet" && segs[1] == "create")
        return send_json(conn, 200, post_wallet_create().second);

    // POST /moderator/release
    if (method == "POST" && segs.size() == 2 && segs[0] == "moderator" && segs[1] == "release")
        return send_json(conn, 200, post_moderator_release(body).second);

    // DELETE /songs/{hash}
    if (method == "DELETE" && segs.size() == 2 && segs[0] == "songs")
        return send_json(conn, 200, delete_song(segs[1], body).second);

    // POST /transactions/transfer
    if (method == "POST" && segs.size() == 2 && segs[0] == "transactions" && segs[1] == "transfer")
        return send_json(conn, 200, post_transfer(body).second);

    // POST /net/announce  (nodes registering themselves so clients can discover them)
    if (method == "POST" && segs.size() == 2 && segs[0] == "net" && segs[1] == "announce")
        return send_json(conn, 200, post_net_announce(body).second);

    // GET /sync/blocks?after_height=N&limit=K  (gossip block pull)
    if (method == "GET" && segs.size() == 2 && segs[0] == "sync" && segs[1] == "blocks")
        return send_json(conn, 200, get_blocks_after(conn).second);

    // POST /sync/block  (gossip block push to local chain)
    if (method == "POST" && segs.size() == 2 && segs[0] == "sync" && segs[1] == "block")
        return send_json(conn, 200, post_sync_block(body).second);

    // bootstrap-file route removed along with the BitTorrent seeder.

    return 0;
}
#endif  // legacy MHD path

// ---- Route implementations -----------------------------------------

std::pair<int, std::string> HttpServer::get_status() {
    auto tip = chain_.tip();
    json j = {
        {"version", "0.1.0"},
        {"block_height", tip.height},
        {"block_hash", crypto::to_hex(tip.hash)},
        {"peer_count", network_.peer_count()},
        {"synced", true},
        {"validator_enabled", config_.validator_enabled},
    };
    return {200, j.dump()};
}

std::pair<int, std::string> HttpServer::get_peers() {
    auto peers = network_.connected_peers();
    json j = json::array();
    for (const auto& p : peers) j.push_back(p);
    return {200, j.dump()};
}

std::pair<int, std::string> HttpServer::get_dht_peers() {
    auto entries = network_.get_dht_peers();
    json j = json::array();
    for (const auto& e : entries) {
        j.push_back({
            {"node_id",  crypto::to_hex(e.node_id)},
            {"ipv6",     e.ipv6_str()},
            {"p2p_port", e.p2p_port},
            {"api_port", e.api_port},
            {"api_url",  e.api_url()},
            {"last_seen_ms", e.last_seen_ms},
        });
    }
    // Also include own node info if we have an IPv6
    json own = {
        {"own_ipv6", network_.own_ipv6_str()},
        {"node_id",  crypto::to_hex(config_.node_id)},
    };
    json resp = {{"peers", j}, {"self", own}};
    return {200, resp.dump()};
}

std::pair<int, std::string> HttpServer::get_block(const std::string& hash_hex) {
    Hash256 h;
    if (!crypto::parse_hash256(hash_hex, h))
        return {400, R"({"error":"invalid hash"})"};
    auto block = chain_.get_block(h);
    if (!block) return {404, R"({"error":"block not found"})"};
    json j = {
        {"hash", crypto::to_hex(block->hash())},
        {"height", chain_.get_block_height(h).value_or(0)},
        {"version", block->header.version},
        {"prev_hash", crypto::to_hex(block->header.prev_hash)},
        {"timestamp", block->header.timestamp_ms},
        {"song", {
            {"content_hash", crypto::to_hex(block->song.content_hash)},
            {"title", block->song.title},
            {"artist", block->song.artist},
            {"genre", block->song.genre},
            {"duration_ms", block->song.duration_ms},
        }},
        {"transaction_count", block->transactions.size()},
    };
    return {200, j.dump()};
}

std::pair<int, std::string> HttpServer::get_block_at_height(uint32_t height) {
    auto hash = chain_.get_block_hash(height);
    if (!hash) return {404, R"({"error":"block not found"})"};
    return get_block(crypto::to_hex(*hash));
}

std::pair<int, std::string> HttpServer::get_songs_list() {
    auto hashes = db_.get_all_song_hashes();
    json j = json::array();
    for (const auto& ch : hashes) {
        if (db_.is_song_deleted(ch)) continue;
        auto meta = db_.get_song_meta(ch);
        if (!meta) continue;
        // Moderator category hide lists — block bytes stay on chain, but
        // any match (artist / album / title) is filtered out of the
        // public listing. Reversible: clear the corresponding hide key
        // and the song reappears.
        if (db_.is_hidden_artist(meta->artist)) continue;
        if (db_.is_hidden_album (meta->album))  continue;
        if (db_.is_hidden_title (meta->title))  continue;
        auto state = db_.get_song_state(ch);
        // Include the SHA256 of the compressed fingerprint so clients can
        // do an O(1) "is this hash already on chain?" check without
        // hashing every track locally. The full constellation stays out
        // of songs.list (~400 KB each — would balloon the response).
        auto fp = db_.get_fingerprint(ch);
        std::string fp_hash_hex;
        if (fp) {
            const Hash256 h = crypto::sha256(
                reinterpret_cast<const uint8_t*>(fp->compressed_fingerprint.data()),
                fp->compressed_fingerprint.size());
            fp_hash_hex = crypto::to_hex(h);
        }
        j.push_back({
            {"content_hash",     crypto::to_hex(ch)},
            {"title",            meta->title},
            {"artist",           meta->artist},
            {"genre",            meta->genre},
            {"album",            meta->album},
            {"duration_ms",      meta->duration_ms},
            {"year",             meta->year},
            {"track_number",     meta->track_number},
            {"play_count",       state.play_count},
            {"fingerprint_hash", fp_hash_hex},
        });
    }
    return {200, j.dump()};
}

// get_songs_search(MHD_Connection*) removed — the HTTP/3 dispatcher in
// transport/h3_server.cpp parses ?artist/?genre/?q from the URL and calls
// the verb_songs_search_* variants below directly.

std::pair<int, std::string> HttpServer::verb_songs_search_query(const std::string& q) {
    return _do_songs_search("", "", q);
}
std::pair<int, std::string> HttpServer::verb_songs_search_artist(const std::string& a) {
    return _do_songs_search(a, "", "");
}
std::pair<int, std::string> HttpServer::verb_songs_search_genre(const std::string& g) {
    return _do_songs_search("", g, "");
}

std::pair<int, std::string> HttpServer::_do_songs_search(const std::string& artist,
                                                          const std::string& genre,
                                                          const std::string& q) {
    std::vector<Hash256> candidates;
    bool filtered = false;

    if (!artist.empty()) {
        candidates = db_.get_songs_by_artist(artist);
        filtered = true;
    } else if (!genre.empty()) {
        candidates = db_.get_songs_by_genre(genre);
        filtered = true;
    }

    if (!filtered) {
        candidates = db_.get_all_song_hashes();
    }

    std::string q_str = q;
    // Lowercase query for case-insensitive match
    for (auto& c : q_str) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    json j = json::array();
    for (const auto& ch : candidates) {
        if (db_.is_song_deleted(ch)) continue;
        auto meta = db_.get_song_meta(ch);
        if (!meta) continue;
        if (db_.is_hidden_artist(meta->artist)) continue;
        if (db_.is_hidden_album (meta->album))  continue;
        if (db_.is_hidden_title (meta->title))  continue;
        if (!q_str.empty()) {
            // Check substring match against title/artist/genre
            auto lc = [](std::string s) {
                for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return s;
            };
            if (lc(meta->title).find(q_str) == std::string::npos &&
                lc(meta->artist).find(q_str) == std::string::npos &&
                lc(meta->genre).find(q_str) == std::string::npos) {
                continue;
            }
        }
        auto state = db_.get_song_state(ch);
        j.push_back({
            {"content_hash", crypto::to_hex(ch)},
            {"title",        meta->title},
            {"artist",       meta->artist},
            {"genre",        meta->genre},
            {"album",        meta->album},
            {"duration_ms",  meta->duration_ms},
            {"year",         meta->year},
            {"track_number", meta->track_number},
            {"play_count",   state.play_count},
        });
    }
    return {200, j.dump()};
}

std::pair<int, std::string> HttpServer::get_song(const std::string& content_hash_hex) {
    Hash256 ch;
    if (!crypto::parse_hash256(content_hash_hex, ch))
        return {400, R"({"error":"invalid hash"})"};
    auto state = db_.get_song_state(ch);
    json j = {
        {"content_hash", content_hash_hex},
        {"play_count", state.play_count},
        {"discoverer", crypto::to_checksum_hex(state.discoverer_address)},
    };
    return {200, j.dump()};
}

std::pair<int, std::string> HttpServer::get_balance(const std::string& address_hex) {
    Address addr;
    if (!crypto::parse_address_checksummed(address_hex, addr))
        return {400, R"({"error":"invalid address"})"};
    uint64_t bal = db_.get_balance(addr);
    json j = {{"address", address_hex}, {"balance", Ledger::format_balance(bal)}};
    return {200, j.dump()};
}

std::pair<int, std::string> HttpServer::get_escrow_balance(
    const std::string& address_hex) {
    Address addr;
    if (!crypto::parse_address_checksummed(address_hex, addr))
        return {400, R"({"error":"invalid address"})"};
    // Escrow lives in the regular ledger under a derived address that
    // has no private key — only the moderator can release via
    // post_moderator_release. Surface both the spendable address and
    // the escrow holding address so the artist UI can show both.
    Address escrow = crypto::escrow_address_for(addr);
    uint64_t bal = db_.get_balance(escrow);
    json j = {
        {"address",        crypto::to_checksum_hex(addr)},
        {"escrow_address", crypto::to_checksum_hex(escrow)},
        {"escrow_balance", Ledger::format_balance(bal)},
    };
    return {200, j.dump()};
}

std::pair<int, std::string> HttpServer::post_session_start(const std::string& body) {
    try {
        auto j = json::parse(body);
        std::string ch_hex  = j["content_hash"];
        std::string pl_hex  = j["player_address"];

        Hash256 ch;
        Address pl;
        if (!crypto::parse_hash256(ch_hex, ch)) return {400, R"({"error":"bad content_hash"})"};
        if (!crypto::parse_address_checksummed(pl_hex, pl)) return {400, R"({"error":"bad player_address"})"};

        // Past the 10k-plays cliff, the listener has to burn tokens
        // per play. The required amount is dynamic — zero until the
        // chain hits SUPPLY_FLOOR, then ramps up cubically toward
        // SUPPLY_CAP (see compute_burn_rate). Reject session.start
        // when the player can't afford the burn rather than letting
        // them stream and then bouncing them at session.complete.
        uint64_t play_count = db_.get_play_count(ch);
        if (play_count >= FULL_REWARD_THRESHOLD) {
            const uint64_t supply = db_.get_total_supply();
            const uint64_t burn   = compute_burn_rate(supply);
            if (burn > 0) {
                const uint64_t bal = db_.get_balance(pl);
                if (bal < burn) {
                    std::ostringstream err;
                    err << R"({"error":"insufficient_balance","required":")"
                        << Ledger::format_balance(burn) << R"("})";
                    return {402, err.str()};
                }
            }
        }

        // Generate session_id
        std::string session_id_hex = generate_session_id();

        PlaySession session;
        crypto::parse_hash256(session_id_hex, session.session_id);
        session.content_hash     = ch;
        session.player_address   = pl;
        session.start_timestamp  = now_ms_api();
        session.last_heartbeat   = session.start_timestamp;
        session.heartbeat_count  = 0;

        // Find block_hash for this song (simplified)
        session.block_hash = {};

        {
            std::lock_guard<std::mutex> lk(sessions_mutex_);
            sessions_[session_id_hex] = session;
        }

        json resp = {
            {"session_id", session_id_hex},
            {"block_hash", crypto::to_hex(session.block_hash)},
        };
        return {200, resp.dump()};
    } catch (...) {
        return {400, R"({"error":"invalid request"})"};
    }
}

std::pair<int, std::string> HttpServer::post_session_heartbeat(
    const std::string& session_id, const std::string& body) {
    std::lock_guard<std::mutex> lk(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return {404, R"({"error":"session not found"})"};
    // BUG FIX: previously the heartbeat handler appended samples
    // even AFTER session.complete had landed for the session. The
    // beats never got used (complete already captured the sample
    // vector) but they bloated the in-memory PlaySession until
    // session expiry. Rejecting up front matches the player flow
    // (HeartbeatService.stop() fires on complete).
    if (it->second.completed)
        return {400, R"({"error":"session already completed"})"};

    try {
        const auto j = json::parse(body);
        const uint64_t now = now_ms_api();
        // BUG FIX: cap the per-session heartbeat rate so a malicious
        // client can't flood thousands of beats per second to inflate
        // the density gate at session.complete time. The legitimate
        // 5 s cadence sits well above kMinIntervalMs.
        constexpr uint64_t kMinIntervalMs = 1000;
        if (!it->second.samples.empty()) {
            const uint64_t last_wall = it->second.samples.back().wall_ms;
            if (now < last_wall + kMinIntervalMs)
                return {429, R"({"error":"heartbeat rate too high"})"};
        }
        // position_ms is the player's claim of "where in the song I
        // am". Missing or non-integer → 400; otherwise sanity check
        // against the song's declared duration if known. Defense in
        // depth — the aggregation loop also clamps by duration, but
        // rejecting at receive time keeps the in-memory sample
        // vector small and surfaces the bad client to the caller.
        constexpr uint64_t kPositionSlackMs = 5000;
        if (!j.contains("position_ms"))
            return {400, R"({"error":"missing position_ms"})"};
        const uint64_t pos_ms = j.value("position_ms",
                                         static_cast<uint64_t>(0));
        const auto height_opt = db_.get_content_height(it->second.content_hash);
        if (height_opt) {
            // BUG FIX: position_ms used to be accepted without bound,
            // so a single beat could carry position_ms = 999999999 and
            // (combined with kPlaybackGraceMs) push effective_ms past
            // any threshold. Now we clamp on receipt instead of relying
            // solely on the song_duration_hint clamp at aggregation
            // time, which only fired when the SongSection actually
            // loaded.
            std::ostringstream fname;
            fname << std::setw(8) << std::setfill('0') << *height_opt << ".blk";
            auto block_path = std::filesystem::path(config_.data_dir)
                            / "blocks" / fname.str();
            std::ifstream bf(block_path, std::ios::binary);
            if (bf) {
                std::vector<uint8_t> fd((std::istreambuf_iterator<char>(bf)), {});
                Block blk;
                if (Block::deserialize(fd.data(), fd.size(), blk)) {
                    const uint64_t dur = blk.song.duration_ms;
                    if (dur > 0 && pos_ms > dur + kPositionSlackMs)
                        return {400, R"({"error":"position_ms past end of song"})"};
                }
            }
        }
        it->second.last_heartbeat = now;
        it->second.heartbeat_count++;
        it->second.samples.push_back({now, pos_ms});
        return {200, R"({"status":"ok"})"};
    } catch (...) {
        return {400, R"({"error":"invalid body"})"};
    }
}

std::pair<int, std::string> HttpServer::post_session_complete(
    const std::string& session_id, const std::string& /*body*/) {
    // BUG FIX: previously we set `it->second.completed = true` here
    // before applying the mint. When the mint failed (any gate
    // rejected, apply_mint returned false, db.write failed), the
    // session was already marked completed and could never be
    // retried — including for transient errors. Now we only flip
    // the flag AFTER mint actually lands; rejected attempts can be
    // retried by the player on next playback once whatever was
    // wrong is fixed.
    PlaySession sess;
    {
        std::lock_guard<std::mutex> lk(sessions_mutex_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) return {404, R"({"error":"session not found"})"};
        if (it->second.completed) return {400, R"({"error":"already completed"})"};
        sess = it->second;
    }
    // Capture session_id so the success path can flip the flag without
    // re-resolving the iterator (the map could have been mutated in
    // the meantime — e.g., session expiry from a later patch).
    const std::string sid_copy = session_id;

    uint64_t now = now_ms_api();
    uint64_t duration_ms = now - sess.start_timestamp;

    // Reject replayed sessions before bothering with the listen-time math.
    if (db_.is_session_used(sess.session_id))
        return {400, R"({"error":"session already used"})"};

    // Load SongSection from block file (need artist_address + royalty_splits,
    // and duration_ms for the 50% listen threshold below).
    SongSection song_section;
    auto height_opt = db_.get_content_height(sess.content_hash);
    if (height_opt) {
        std::ostringstream fname;
        fname << std::setw(8) << std::setfill('0') << *height_opt << ".blk";
        auto block_path = std::filesystem::path(config_.data_dir) / "blocks" / fname.str();
        std::ifstream bf(block_path, std::ios::binary);
        if (bf) {
            std::vector<uint8_t> fd((std::istreambuf_iterator<char>(bf)), {});
            Block blk;
            if (Block::deserialize(fd.data(), fd.size(), blk))
                song_section = blk.song;
        }
    }

    // effective_ms = union of timestamp ranges within the song that the
    // listener actually played. Re-listening to the same chorus three
    // times counts once; skipping forward to a new section adds that
    // section's length. The threshold below compares this against the
    // song's duration so "completed enough of the song" is robust to
    // seeking around without farming play credit by replaying a 5 s clip
    // until the wall clock hits 30 s.
    //
    // Wall-time → song-time projection: between two consecutive
    // heartbeats the listener is presumed to have played [a.position_ms,
    // a.position_ms + min(wall_dt + grace, song_duration_ms)]. For a
    // monotonic forward-playing listener that equals their next reported
    // position. For paused / buffering windows it's bounded by wall_dt
    // so a long pause doesn't synthesize coverage. Backward seeks
    // produce a new range starting at the new position rather than
    // continuing the old one.
    // BUG FIX: kPlaybackGraceMs used to be 2000 — 100 samples
    // synthesised 200 s of listening that didn't happen. 500 ms is
    // generous enough to cover RPC / scheduling jitter on a 5 s
    // cadence without padding accumulated ranges.
    constexpr uint64_t kPlaybackGraceMs   = 500;
    // BUG FIX: cap per-sample advance at twice the expected
    // HeartbeatService cadence (10 s). The previous code only
    // capped on the next sample's position delta or song duration;
    // for the LAST sample (no next sample) wall_dt = now -
    // last_heartbeat, which could be 30+ s if the player paused
    // before completing — that pause time was credited as listening
    // time. Capping at 2× cadence kills the rubber-band.
    constexpr uint64_t kMaxAdvancePerSampleMs = 10000;
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    const auto& samples = sess.samples;
    const uint64_t song_duration_ms_hint = song_section.duration_ms;
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto& a = samples[i];
        const uint64_t next_wall = (i + 1 < samples.size())
            ? samples[i + 1].wall_ms : now;
        // BUG FIX: previously `continue` on out-of-order — that
        // silently dropped the sample without recording even its
        // own position point. Instead clamp the slice to zero
        // advance so the sample still contributes its (a.position_ms,
        // a.position_ms+0) range, which the union code below treats
        // as a no-op but at least doesn't fall behind on counts.
        uint64_t wall_dt = next_wall >= a.wall_ms
            ? next_wall - a.wall_ms : 0;
        uint64_t advance = std::min<uint64_t>(
            wall_dt + kPlaybackGraceMs, kMaxAdvancePerSampleMs);
        // If we have a next heartbeat and it reports a higher
        // position for the same wall slice, that's the listener's
        // authoritative claim — bound advance by it. (Seeking
        // forward DURING the slice is still bounded by wall_dt+grace
        // and per-sample cap above, preventing skip-farming.)
        if (i + 1 < samples.size()) {
            const auto& b = samples[i + 1];
            if (b.position_ms > a.position_ms) {
                advance = std::min(advance, b.position_ms - a.position_ms);
            } else {
                // Seek back / no progress — this slice contributes
                // nothing new beyond the single-point position
                // itself.
                advance = 0;
            }
        }
        if (song_duration_ms_hint > 0 && a.position_ms < song_duration_ms_hint) {
            advance = std::min<uint64_t>(advance,
                song_duration_ms_hint - a.position_ms);
        }
        if (advance == 0) continue;
        ranges.emplace_back(a.position_ms, a.position_ms + advance);
    }
    // Sort + merge so re-listened ranges collapse to their union.
    std::sort(ranges.begin(), ranges.end());
    uint64_t effective_ms = 0;
    uint64_t cur_start = 0, cur_end = 0;
    bool have_cur = false;
    for (const auto& r : ranges) {
        if (!have_cur) {
            cur_start = r.first; cur_end = r.second; have_cur = true;
            continue;
        }
        if (r.first <= cur_end) {
            if (r.second > cur_end) cur_end = r.second;
        } else {
            effective_ms += cur_end - cur_start;
            cur_start = r.first; cur_end = r.second;
        }
    }
    if (have_cur) effective_ms += cur_end - cur_start;

    // Two gates the play has to clear before the chain mints. All
    // must hold; any failure returns 400 and logs a
    // [session.complete] REJECT line with the failing reason.
    //
    //  (1) Enough timestamps. The HeartbeatService ticks every 5 s;
    //      we accept down to one beat per 10 s of wall time AND a
    //      hard floor of 6 beats overall. A "press play, immediately
    //      press complete" loop with one or two beats can't claim a
    //      play.
    //
    //  (2) Timestamps cover the song. The set of reported position_ms
    //      values has to span enough of the song to make the claim
    //      credible. When the chain knows the song's duration we
    //      require positions covering ≥ 50 % of it (so a 5 s loop
    //      replayed for the wall-clock equivalent of the song still
    //      fails — same-range union collapses identical ranges). When
    //      the song isn't registered yet (duration_ms = 0) we fall
    //      back to ≥ 30 s of distinct content; the next block lands
    //      the duration and subsequent plays use the 50 % path.
    //
    // Heartbeats are allowed to be perfectly periodic — the
    // position_ms span gate is what makes a real listen
    // indistinguishable from a "looks human" play.
    constexpr uint64_t kPlayPercentRequired = 50;
    constexpr uint64_t kLegacyMinListenMs   = 30000;
    constexpr uint64_t kMinHeartbeats       = 6;
    constexpr uint64_t kMaxMsPerHeartbeat   = 10000;

    auto reject = [&](const std::string& err_json,
                      const std::string& reason) {
        std::cout << "[session.complete] REJECT sid="
                  << crypto::to_hex(sess.session_id).substr(0, 12)
                  << " reason=" << reason
                  << " eff_ms=" << effective_ms
                  << " song_dur_ms=" << song_section.duration_ms
                  << " heartbeats=" << samples.size()
                  << " wall_ms=" << duration_ms << "\n";
        return std::pair<int, std::string>{400, err_json};
    };

    // ---- gate 1: enough timestamps ----------------------------------
    {
        const uint64_t density_min =
            std::max<uint64_t>(kMinHeartbeats,
                               duration_ms / kMaxMsPerHeartbeat);
        if (samples.size() < density_min) {
            std::ostringstream err;
            err << R"({"error":"sparse heartbeats","heartbeats":)"
                << samples.size()
                << R"(,"required_heartbeats":)" << density_min
                << R"(,"wall_duration_ms":)" << duration_ms << "}";
            return reject(err.str(), "sparse_heartbeats");
        }
    }

    // ---- gate 2: timestamps cover the song --------------------------
    {
        const uint64_t required_ms = song_section.duration_ms > 0
            ? (uint64_t{song_section.duration_ms}
                * kPlayPercentRequired / 100)
            : kLegacyMinListenMs;
        if (effective_ms < required_ms) {
            std::ostringstream err;
            err << R"({"error":"position_ms timestamps don't cover the song","effective_listened_ms":)"
                << effective_ms
                << R"(,"required_ms":)" << required_ms
                << R"(,"song_duration_ms":)" << song_section.duration_ms
                << R"(,"required_percent":)" << kPlayPercentRequired
                << R"(,"wall_duration_ms":)" << duration_ms << "}";
            return reject(err.str(), "below_threshold");
        }
    }

    // Build PlayProof
    PlayProof proof;
    proof.session_id           = sess.session_id;
    proof.content_hash         = sess.content_hash;
    proof.block_hash           = sess.block_hash;
    proof.artist_address       = song_section.artist_address;
    proof.player_address       = sess.player_address;
    proof.serving_node_id      = config_.node_id;
    proof.play_start_timestamp = sess.start_timestamp;
    proof.play_end_timestamp   = now;
    proof.total_duration_ms    = (duration_ms > 0xFFFFFFFFu)
        ? 0xFFFFFFFFu : static_cast<uint32_t>(duration_ms);
    proof.heartbeat_count      = (sess.heartbeat_count > 0xFFFFu)
        ? static_cast<uint16_t>(0xFFFF) : static_cast<uint16_t>(sess.heartbeat_count);

    // Node signs the proof
    auto sign_msg = proof.sign_message();
    Hash256 sh    = crypto::sha256(sign_msg.data(), sign_msg.size());
    proof.node_signature = crypto::sign_ecdsa(sh, node_keypair_.private_key);

    // Compute mint outputs
    uint64_t play_count = db_.get_play_count(sess.content_hash);
    Address  node_addr  = node_keypair_.address;
    auto outputs = compute_mint_outputs(proof, song_section, play_count,
                                        config_.node_id, node_addr);

    // Burn rate scales with total minted supply; zero until the chain
    // reaches SUPPLY_FLOOR, growing cubically to "hyperdrive" near
    // SUPPLY_CAP. Below the 10k-plays threshold there's no burn at all
    // (the listener is in discovery tier).
    MintTx mint;
    mint.proof       = proof;
    mint.outputs     = outputs;
    mint.burn_amount = (play_count >= FULL_REWARD_THRESHOLD)
        ? compute_burn_rate(db_.get_total_supply())
        : 0;

    // Apply mint directly to chain state
    leveldb::WriteBatch batch;
    if (!chain_.apply_mint(mint, play_count, batch)) {
        std::cout << "[session.complete] APPLY-MINT-FAIL sid="
                  << crypto::to_hex(sess.session_id).substr(0, 12) << "\n";
        return {500, R"({"error":"failed to apply mint"})"};
    }
    if (!db_.write(batch)) {
        std::cout << "[session.complete] DB-WRITE-FAIL sid="
                  << crypto::to_hex(sess.session_id).substr(0, 12) << "\n";
        return {500, R"({"error":"db write failed"})"};
    }
    // Mint actually landed. NOW flip the in-memory completed flag so
    // a retry returns "already completed" instead of double-minting.
    {
        std::lock_guard<std::mutex> lk(sessions_mutex_);
        auto it = sessions_.find(sid_copy);
        if (it != sessions_.end()) it->second.completed = true;
    }
    std::cout << "[session.complete] OK sid="
              << crypto::to_hex(sess.session_id).substr(0, 12)
              << " player=" << crypto::to_checksum_hex(sess.player_address)
              << " artist=" << crypto::to_checksum_hex(song_section.artist_address)
              << " play_count=" << (play_count + 1)
              << " outputs=" << outputs.size()
              << " eff_ms=" << effective_ms
              << " heartbeats=" << samples.size() << "\n";

    // Tally response amounts
    uint64_t artist_amount = 0, node_amount = 0, discoverer_amount = 0;
    for (const auto& out : outputs) {
        if (out.recipient == song_section.artist_address) artist_amount    += out.amount;
        else if (out.recipient == node_addr)              node_amount      += out.amount;
        else                                              discoverer_amount += out.amount;
    }

    SongState new_state = db_.get_song_state(sess.content_hash);
    bool is_discoverer  = (new_state.play_count == 1); // this was the first play

    json resp = {
        {"status", "ok"},
        {"play_count", new_state.play_count},
        {"is_discoverer", is_discoverer},
        {"tokens_minted", {
            {"artist_amount",     Ledger::format_balance(artist_amount)},
            {"node_amount",       Ledger::format_balance(node_amount)},
            {"discoverer_amount", Ledger::format_balance(discoverer_amount)},
        }},
    };
    return {200, resp.dump()};
}

// ---- Wallet routes --------------------------------------------------

std::pair<int, std::string> HttpServer::get_wallet_address() {
    json j = {{"address", crypto::to_checksum_hex(node_keypair_.address)}};
    return {200, j.dump()};
}

std::pair<int, std::string> HttpServer::get_wallet_nonce(const std::string& address_hex) {
    Address addr;
    if (!crypto::parse_address_checksummed(address_hex, addr))
        return {400, R"({"error":"invalid address"})"};
    uint64_t nonce = db_.get_nonce(addr);
    json j = {{"address", crypto::to_checksum_hex(addr)}, {"nonce", nonce}};
    return {200, j.dump()};
}

std::pair<int, std::string> HttpServer::post_wallet_create() {
    auto kp = crypto::generate_keypair();
    json j = {
        {"address",     crypto::to_checksum_hex(kp.address)},
        {"public_key",  crypto::to_hex(kp.public_key.data(), 33)},
        {"private_key", crypto::to_hex(kp.private_key.data(), kp.private_key.size())},
    };
    return {200, j.dump()};
}

// ---- Moderator routes -----------------------------------------------

static bool verify_moderator_sig(const std::string& mod_addr_hex,
                                  const std::string& sig_hex,
                                  const std::string& sign_msg,
                                  Database& db) {
    Address mod_addr;
    if (!crypto::parse_address_checksummed(mod_addr_hex, mod_addr)) return false;
    if (!db.is_moderator(mod_addr)) return false;
    auto sig_bytes = crypto::from_hex(sig_hex);
    if (sig_bytes.size() != 64) return false;
    Sig64 sig;
    std::copy(sig_bytes.begin(), sig_bytes.end(), sig.begin());
    Hash256 hash = crypto::sha256(reinterpret_cast<const uint8_t*>(sign_msg.data()),
                                   sign_msg.size());
    return crypto::verify_ecdsa_from_address(hash, sig, mod_addr);
}

std::pair<int, std::string> HttpServer::post_moderator_release(const std::string& body) {
    try {
        auto j = json::parse(body);
        std::string mod_addr_hex = j["moderator_address"];
        std::string mod_sig_hex  = j["moderator_signature"];
        std::string from_hex_str = j["from_address"];
        std::string to_hex_str   = j["to_address"];
        std::string amount_str   = j["amount"];

        std::string sign_msg = from_hex_str + to_hex_str + amount_str;
        if (!verify_moderator_sig(mod_addr_hex, mod_sig_hex, sign_msg, db_))
            return {403, R"({"error":"unauthorized"})"};

        Address from_addr, to_addr;
        if (!crypto::parse_address_checksummed(from_hex_str, from_addr))
            return {400, R"({"error":"bad from_address"})"};
        if (!crypto::parse_address_checksummed(to_hex_str, to_addr))
            return {400, R"({"error":"bad to_address"})"};

        uint64_t amount = 0;
        if (!Ledger::parse_balance(amount_str, amount))
            return {400, R"({"error":"bad amount"})"};

        leveldb::WriteBatch batch;
        Ledger ledger(db_);
        if (!ledger.transfer(batch, from_addr, to_addr, amount))
            return {400, R"({"error":"insufficient balance"})"};
        db_.write(batch);

        return {200, R"({"status":"ok"})"};
    } catch (...) {
        return {400, R"({"error":"invalid request"})"};
    }
}

std::pair<int, std::string> HttpServer::delete_song(const std::string& content_hash_hex,
                                                      const std::string& body) {
    try {
        auto j = json::parse(body);
        std::string mod_addr_hex = j["moderator_address"];
        std::string mod_sig_hex  = j["moderator_signature"];

        std::string sign_msg = "delete:" + content_hash_hex;
        if (!verify_moderator_sig(mod_addr_hex, mod_sig_hex, sign_msg, db_))
            return {403, R"({"error":"unauthorized"})"};

        Hash256 ch;
        if (!crypto::parse_hash256(content_hash_hex, ch))
            return {400, R"({"error":"bad content_hash"})"};

        leveldb::WriteBatch batch;
        db_.mark_song_deleted(batch, ch);
        db_.write(batch);

        return {200, R"({"status":"ok"})"};
    } catch (...) {
        return {400, R"({"error":"invalid request"})"};
    }
}

// ---- Transfer route -------------------------------------------------

std::pair<int, std::string> HttpServer::post_transfer(const std::string& body) {
    try {
        auto j = json::parse(body);
        std::string from_hex_str = j["from_address"];
        std::string to_hex_str   = j["to_address"];
        std::string amount_str   = j["amount"];
        uint64_t    nonce        = j["nonce"].get<uint64_t>();
        std::string sig_hex      = j["signature"];
        std::string pub_hex      = j.value("from_pubkey", std::string());

        Address from_addr, to_addr;
        if (!crypto::parse_address_checksummed(from_hex_str, from_addr))
            return {400, R"({"error":"bad from_address"})"};
        if (!crypto::parse_address_checksummed(to_hex_str, to_addr))
            return {400, R"({"error":"bad to_address"})"};

        uint64_t amount = 0;
        if (!Ledger::parse_balance(amount_str, amount))
            return {400, R"({"error":"bad amount"})"};

        auto sig_bytes = crypto::from_hex(sig_hex);
        if (sig_bytes.size() != 64)
            return {400, R"({"error":"bad signature"})"};

        // from_pubkey is required now that verify_signature cross-checks
        // the inline pubkey against from_address (no ECDSA recovery).
        auto pub_bytes = crypto::from_hex(pub_hex);
        if (pub_bytes.size() != 33)
            return {400, R"({"error":"bad from_pubkey"})"};

        TransferTx tx;
        tx.from_address = from_addr;
        tx.to_address   = to_addr;
        tx.amount       = amount;
        tx.nonce        = nonce;
        std::copy(pub_bytes.begin(), pub_bytes.end(), tx.from_pubkey.begin());
        std::copy(sig_bytes.begin(), sig_bytes.end(), tx.signature.begin());

        leveldb::WriteBatch batch;
        if (!chain_.apply_transfer(tx, batch))
            return {400, R"({"error":"transfer rejected"})"};
        db_.write(batch);

        auto raw = tx.serialize();
        auto tx_hash = tx.tx_hash();
        db_.put_pending_tx(tx_hash, raw);
        // TX broadcast over the legacy TCP mesh removed in Phase 2c. When
        // we have more than one full node, the broadcast will be done over
        // mc_rats_quic via rats_broadcast_message.

        json resp = {{"status", "ok"}, {"tx_hash", crypto::to_hex(tx_hash)}};
        return {200, resp.dump()};
    } catch (...) {
        return {400, R"({"error":"invalid request"})"};
    }
}

std::pair<int, std::string> HttpServer::post_net_announce(const std::string& body) {
    try {
        auto j = json::parse(body);
        std::string ipv6        = j.value("ipv6",     "");
        uint16_t    api_port    = static_cast<uint16_t>(j.value("api_port", 0));
        std::string node_id_hex = j.value("node_id",  "");

        if (node_id_hex.size() != 64)
            return {400, R"({"error":"invalid node_id"})"};
        if (ipv6.empty())
            return {400, R"({"error":"ipv6 required"})"};

        Hash256 node_id{};
        if (!crypto::parse_hash256(node_id_hex, node_id))
            return {400, R"({"error":"invalid node_id"})"};

        network_.inject_peer(ipv6, api_port, node_id);
        return {200, R"({"status":"ok"})"};
    } catch (...) {
        return {400, R"({"error":"invalid json"})"};
    }
}

// ---- Block sync routes ----------------------------------------------

// Block sync via HTTP removed — moves to rats binary chunks later.
#if 0
static std::pair<int, std::string> _legacy_get_blocks_after(uint32_t after_height, uint32_t limit) { (void)after_height; (void)limit; return {200, "[]"}; }
#endif

std::pair<int, std::string> HttpServer::post_sync_block(const std::string& body) {
    try {
        auto j = json::parse(body);
        std::string raw_hex = j.value("raw_hex", "");
        if (raw_hex.empty())
            return {400, R"({"error":"raw_hex required"})"};

        auto raw = crypto::from_hex(raw_hex);
        if (raw.empty())
            return {400, R"({"error":"invalid hex"})"};

        Block block;
        if (!Block::deserialize(raw.data(), raw.size(), block))
            return {400, R"({"error":"block deserialization failed"})"};

        std::string err;
        if (!chain_.validate_block(block, err))
            return {400, json{{"error", "block validation failed"}, {"detail", err}}.dump()};

        // Write .blk file before connecting (at expected next height)
        uint32_t new_height = chain_.tip().height + 1;
        std::ostringstream fname;
        fname << std::setw(8) << std::setfill('0') << new_height << ".blk";
        auto blocks_dir = std::filesystem::path(config_.data_dir) / "blocks";
        std::filesystem::create_directories(blocks_dir);
        auto block_path = blocks_dir / fname.str();
        {
            std::ofstream f(block_path, std::ios::binary);
            f.write(reinterpret_cast<const char*>(raw.data()), raw.size());
        }

        if (!chain_.connect_block(block)) {
            std::filesystem::remove(block_path);
            return {400, R"({"error":"block rejected by chain"})"};
        }

        std::cout << "[sync] connected block at height " << chain_.tip().height << "\n";
        return {200, R"({"status":"ok"})"};
    } catch (...) {
        return {400, R"({"error":"invalid request"})"};
    }
}


// ---- Helpers --------------------------------------------------------

// send_json / send_binary removed — HTTP/3 responses are emitted by
// transport/h3_server.cpp via MsH3RequestSend(). The dispatcher there
// reads the verb's {status, json-or-bytes, content_type} tuple and ships it.

std::string HttpServer::generate_session_id() const {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    Hash256 id{};
    for (int i = 0; i < 4; ++i) {
        uint64_t v = dist(gen);
        for (int j = 0; j < 8; ++j) id[i*8+j] = (v >> (j*8)) & 0xFF;
    }
    return crypto::to_hex(id);
}

} // namespace mc::api
