// main.cpp — bopwire-web-gateway.
//
// HTTPS/JSON façade (behind Caddy) over the librats network. Serves the Discover
// feed, streams audio pulled from the swarm, and runs honest play sessions so a
// web play mints the artist/seeder/mini reward — never the listener.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "httplib.h"
#include "json.hpp"

#include "rats_link.h"
#include "streamer.h"

using namespace bopwire::gw;
using json = nlohmann::json;

// ─────────────────────────── config ───────────────────────────
static std::string env_or(const char* k, const std::string& d) {
    const char* v = std::getenv(k);
    return (v && *v) ? std::string(v) : d;
}
static int env_int(const char* k, int d) {
    const char* v = std::getenv(k);
    return (v && *v) ? std::atoi(v) : d;
}

// ─────────────────────────── helpers ──────────────────────────
static std::string random_addr() {
    static std::mutex m;
    static std::mt19937_64 rng(std::random_device{}());
    std::lock_guard<std::mutex> lk(m);
    static const char* hex = "0123456789abcdef";
    std::string s; s.reserve(40);
    for (int i = 0; i < 40; ++i) s.push_back(hex[rng() & 0xF]);
    return s;
}

static json normalize_song(const json& s) {
    return json{
        {"contentHash", s.value("content_hash", "")},
        {"title",       s.value("title", "")},
        {"artist",      s.value("artist", "")},
        {"album",       s.value("album", "")},
        {"genre",       s.value("genre", "")},
        {"year",        s.value("year", 0)},
        {"trackNumber", s.value("track_number", 0)},
        {"durationMs",  s.value("duration_ms", 0)},
        {"playCount",   s.value("play_count", 0)},
        {"swarmSize",   s.value("swarm_size", 0)},
    };
}

// ───────────────── open-stream (PieceStore) cache ─────────────────
// One opened PieceStore per song, reused across the browser's range requests
// (so seeks/re-reads hit cached pieces). Concurrent first-opens are de-duped;
// the least-recently-opened store is evicted past the cap.
class StoreCache {
public:
    StoreCache(RatsLink& link, size_t max_stores) : link_(link), cap_(max_stores) {}

    std::shared_ptr<PieceStore> peek(const std::string& h) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = map_.find(h);
        return it == map_.end() ? nullptr : it->second;
    }

    // Opened store (cached) or nullptr if it can't open (no swarm). Blocking on
    // first open (stream.open + first piece); instant on subsequent calls.
    std::shared_ptr<PieceStore> get(const std::string& h) {
        if (auto s = peek(h)) return s;
        std::shared_future<std::shared_ptr<PieceStore>> fut;
        bool owner = false;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (auto it = map_.find(h); it != map_.end()) return it->second;
            if (auto it = inflight_.find(h); it != inflight_.end()) fut = it->second;
            else {
                owner = true;
                RatsLink* lk = &link_;
                fut = std::async(std::launch::async, [lk, h] {
                    auto ps = std::make_shared<PieceStore>(*lk, h);
                    return ps->open() ? ps : std::shared_ptr<PieceStore>();
                }).share();
                inflight_[h] = fut;
            }
        }
        std::shared_ptr<PieceStore> ps;
        try { ps = fut.get(); } catch (...) { ps = nullptr; }
        if (owner) {
            std::lock_guard<std::mutex> lk(m_);
            inflight_.erase(h);
            if (ps) {
                map_[h] = ps; order_.push_back(h);
                while (order_.size() > cap_) { map_.erase(order_.front()); order_.pop_front(); }
            }
        }
        return ps;
    }
private:
    RatsLink& link_;
    std::mutex m_;
    std::unordered_map<std::string, std::shared_ptr<PieceStore>> map_;
    std::unordered_map<std::string, std::shared_future<std::shared_ptr<PieceStore>>> inflight_;
    std::deque<std::string> order_;
    size_t cap_;
};

// ─────────────────────────── main ─────────────────────────────
int main() {
    const std::string vps_host = env_or("BOPWIRE_VPS_HOST", "127.0.0.1");
    const int         vps_port = env_int("BOPWIRE_VPS_PORT", 8080);
    const std::string gw_id    = env_or("BOPWIRE_GATEWAY_ID", "");       // 40-hex or auto
    const std::string host     = env_or("BOPWIRE_LISTEN_HOST", "127.0.0.1");
    const int         port     = env_int("BOPWIRE_LISTEN_PORT", 8090);
    const size_t      max_streams = (size_t) env_int("BOPWIRE_MAX_STREAMS", 24);

    // Allowed CORS origins (comma-separated).
    std::vector<std::string> origins;
    {
        std::string raw = env_or("BOPWIRE_ALLOWED_ORIGINS",
                                 "https://bopwire.com,https://www.bopwire.com");
        size_t p = 0, c;
        while ((c = raw.find(',', p)) != std::string::npos) {
            origins.push_back(raw.substr(p, c - p)); p = c + 1;
        }
        origins.push_back(raw.substr(p));
    }

    RatsLink link(vps_host, vps_port, gw_id);
    if (!link.start()) { std::fprintf(stderr, "[gw] could not join the network\n"); return 1; }
    StoreCache stores(link, max_streams);

    const auto t0 = std::chrono::steady_clock::now();

    // catalog cache (coalesce repeated songs.list)
    std::mutex cat_mu;
    json cat_cache = json::array();
    std::chrono::steady_clock::time_point cat_at{};

    auto load_catalog = [&]() -> json {
        std::lock_guard<std::mutex> lk(cat_mu);
        auto now = std::chrono::steady_clock::now();
        if (!cat_cache.empty() &&
            now - cat_at < std::chrono::seconds(8)) return cat_cache;
        const std::string node = link.pick_full_node();
        if (node.empty()) throw std::runtime_error("no_node");
        json r = link.rpc_via_relay(node, "songs.list", json::object(), 15000);
        json arr = json::array();
        for (const auto& s : r.value("body", json::array()))
            if (s.value("swarm_size", 0) > 0) arr.push_back(normalize_song(s));
        cat_cache = arr; cat_at = now;
        return arr;
    };

    // active play sessions (reward lifecycle)
    struct Play { std::string hash, node; };
    std::mutex play_mu;
    std::unordered_map<std::string, Play> plays;

    // ───────────────────────── HTTP ─────────────────────────
    httplib::Server svr;

    auto cors = [&](const httplib::Request& req, httplib::Response& res) {
        std::string origin = req.get_header_value("Origin");
        std::string allow = origins.empty() ? "*" : origins.front();
        for (const auto& o : origins) if (o == origin || o == "*") { allow = origin.empty() ? o : origin; break; }
        res.set_header("Access-Control-Allow-Origin", allow);
        res.set_header("Vary", "Origin");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
    };
    svr.set_post_routing_handler([&](const httplib::Request& req, httplib::Response& res) {
        cors(req, res);
    });
    svr.Options(R"(/.*)", [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

    auto err = [](httplib::Response& res, int code, const std::string& msg) {
        res.status = code;
        res.set_content(json{{"error", msg}}.dump(), "application/json");
    };

    svr.Get("/api/health", [&](const httplib::Request&, httplib::Response& res) {
        auto up = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::steady_clock::now() - t0).count();
        json out{{"ok", true},
                 {"node", link.pick_full_node().empty() ? "connecting" : "connected"},
                 {"uptime_s", up}};
        res.set_content(out.dump(), "application/json");
    });

    svr.Get("/api/songs", [&](const httplib::Request&, httplib::Response& res) {
        try { res.set_content(load_catalog().dump(), "application/json"); }
        catch (const std::exception& e) { err(res, 503, e.what()); }
    });

    svr.Get("/api/search", [&](const httplib::Request& req, httplib::Response& res) {
        json body = json::object();
        if (req.has_param("q"))      body["q"]      = req.get_param_value("q");
        if (req.has_param("artist")) body["artist"] = req.get_param_value("artist");
        if (req.has_param("genre"))  body["genre"]  = req.get_param_value("genre");
        try {
            const std::string node = link.pick_full_node();
            if (node.empty()) { err(res, 503, "no_node"); return; }
            json r = link.rpc_via_relay(node, "songs.search", body, 15000);
            json arr = json::array();
            for (const auto& s : r.value("body", json::array()))
                if (s.value("swarm_size", 0) > 0) arr.push_back(normalize_song(s));
            res.set_content(arr.dump(), "application/json");
        } catch (const std::exception& e) { err(res, 503, e.what()); }
    });

    // GET /api/stream/<64-hex> — audio, streamed progressively from the swarm.
    // httplib's content provider handles Range (206) by driving `offset`; we
    // pull ~256 KB per call from the on-demand PieceStore so first-byte latency
    // is one piece fetch, not the whole file.
    svr.Get(R"(/api/stream/([0-9a-fA-F]{64}))",
            [&](const httplib::Request& req, httplib::Response& res) {
        const std::string h = req.matches[1];
        auto store = stores.get(h);
        if (!store || store->total_size() <= 0) {
            err(res, 404, "no seeders for this song right now"); return;
        }
        res.set_header("Accept-Ranges", "bytes");
        auto sp = store;   // keep the store alive for the streamed response
        res.set_content_provider(
            (size_t) store->total_size(), store->content_type(),
            [sp](size_t offset, size_t length, httplib::DataSink& sink) -> bool {
                const size_t want = std::min<size_t>(length, 256 * 1024);
                std::string chunk = sp->get_range((int64_t) offset, (int64_t) want);
                if (chunk.empty()) return false;          // EOF or fetch failure
                return sink.write(chunk.data(), chunk.size());
            });
    });

    // ── reward lifecycle (browser reports REAL playback) ──
    svr.Post("/api/play/start", [&](const httplib::Request& req, httplib::Response& res) {
        json in; try { in = json::parse(req.body); } catch (...) { err(res, 400, "bad json"); return; }
        const std::string h = in.value("contentHash", "");
        if (h.size() != 64) { err(res, 400, "bad contentHash"); return; }
        const std::string node = link.pick_full_node();
        if (node.empty()) { err(res, 503, "no_node"); return; }
        try {
            json r = link.rpc_via_relay(node, "session.start", json{
                {"content_hash", h},
                {"player_address", random_addr()},  // ephemeral listener; earns nothing
                {"attestation", json::object()},
            }, 12000);
            const std::string sid = r.value("body", json::object()).value("session_id", "");
            if (sid.empty()) { err(res, 502, "no session_id"); return; }
            { std::lock_guard<std::mutex> lk(play_mu); plays[sid] = Play{h, node}; }
            res.set_content(json{{"playId", sid}}.dump(), "application/json");
        } catch (const std::exception& e) { err(res, 502, e.what()); }
    });

    svr.Post("/api/play/heartbeat", [&](const httplib::Request& req, httplib::Response& res) {
        json in; try { in = json::parse(req.body); } catch (...) { err(res, 400, "bad json"); return; }
        const std::string sid = in.value("playId", "");
        const int64_t pos = in.value("positionMs", 0);
        Play pl; { std::lock_guard<std::mutex> lk(play_mu); auto it = plays.find(sid);
                   if (it == plays.end()) { err(res, 404, "no such play"); return; } pl = it->second; }
        try {
            link.rpc_via_relay(pl.node, "session.heartbeat",
                               json{{"session_id", sid}, {"position_ms", pos}}, 8000);
            res.set_content("{\"ok\":true}", "application/json");
        } catch (const std::exception& e) { err(res, 502, e.what()); }
    });

    svr.Post("/api/play/complete", [&](const httplib::Request& req, httplib::Response& res) {
        json in; try { in = json::parse(req.body); } catch (...) { err(res, 400, "bad json"); return; }
        const std::string sid = in.value("playId", "");
        Play pl; { std::lock_guard<std::mutex> lk(play_mu); auto it = plays.find(sid);
                   if (it == plays.end()) { err(res, 404, "no such play"); return; }
                   pl = it->second; plays.erase(it); }
        json body{{"session_id", sid}};
        if (auto st = stores.peek(pl.hash)) {         // reward lanes from the stream
            if (!st->seeder().empty()) body["seeder_address"]    = st->seeder();
            if (!st->mini().empty())   body["mini_node_address"] = st->mini();
        }
        try {
            json r = link.rpc_via_relay(pl.node, "session.complete", body, 12000);
            res.set_content(r.value("body", json::object()).dump(), "application/json");
        } catch (const std::exception& e) { err(res, 502, e.what()); }
    });

    std::printf("[gw] listening on http://%s:%d  (%zu mini(s) in mesh)\n",
                host.c_str(), port, link.mini_count());
    svr.listen(host.c_str(), port);
    link.stop();
    return 0;
}
