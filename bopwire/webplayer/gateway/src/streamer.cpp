// streamer.cpp — see streamer.h.
#include "streamer.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace bopwire::gw {

namespace {
constexpr int    kPieceSize     = 256 * 1024;   // matches the app default
constexpr int    kCountPerFetch = 16;           // == kMaxSwarmFetchPieces
constexpr size_t kDefaultMaxBytes = 256ull * 1024 * 1024;  // 256 MB safety cap

// Generous per-range wait: a single seeder flow is capped at 500 KB/s, so size
// the timeout off a conservative 200 KB/s floor plus fixed slack.
int wait_ms_for(int64_t range_len) {
    return static_cast<int>(range_len / 200 + 8000);
}
} // namespace

FetchResult Streamer::fetch(const std::string& content_hash, size_t max_bytes) {
    if (max_bytes == 0) max_bytes = kDefaultMaxBytes;
    FetchResult out;

    const std::string node = link_.pick_full_node();
    if (node.empty()) { out.error = "no_node"; return out; }

    // Pin ONE mini for this whole play (least-loaded). All of this song's relay
    // traffic — stream.open + every swarm.fetch — goes through it, so load spreads
    // across the mesh per-play rather than hammering a single mini.
    const std::string mini = link_.pick_mini();
    if (mini.empty()) { out.error = "no_mini"; return out; }

    // 1) stream.open → seeders + delivery_id + manifest
    json open;
    try {
        open = link_.rpc_relay(mini, node, "stream.open",
                               json{{"content_hash", content_hash}}, 10000);
    } catch (const std::exception& e) {
        out.error = std::string("stream_open_failed: ") + e.what();
        return out;
    }
    const json body = open.value("body", json::object());
    const std::string status = open.value("status", body.value("status", ""));
    if (status == "unknown") { out.error = "unknown"; return out; }

    std::vector<std::string> seeders;
    for (const auto& p : body.value("peers", json::array())) {
        if (p.is_string()) seeders.push_back(p.get<std::string>());
        else if (p.is_object()) {
            std::string pid = p.value("peer_id", "");
            if (!pid.empty()) seeders.push_back(pid);
        }
    }
    if (seeders.empty()) { out.error = "no_swarm"; return out; }

    out.delivery_id = body.value("delivery_id", "");
    int piece_size = kPieceSize;
    if (body.contains("manifest") && body["manifest"].is_object())
        piece_size = body["manifest"].value("piece_size", kPieceSize);
    if (piece_size <= 0 || piece_size > 512 * 1024) piece_size = kPieceSize;

    // 2) fetch ranges, resuming across seeders on failure
    std::string assembled;
    int64_t total = -1;
    int piece_start = 0;

    while (total < 0 || static_cast<int64_t>(assembled.size()) < total) {
        bool got = false;
        for (const std::string& seeder : seeders) {
            const uint32_t csid = link_.next_stream_id();
            auto sink = link_.register_stream(csid);

            json fr;
            try {
                fr = link_.rpc_relay(mini, seeder, "swarm.fetch", json{
                    {"v", 1},
                    {"content_hash", content_hash},
                    {"piece_size", piece_size},
                    {"piece_start", piece_start},
                    {"count", kCountPerFetch},
                    {"delivery_id", out.delivery_id},
                    {"client_stream_id", csid},
                }, 12000);
            } catch (...) { link_.unregister_stream(csid); continue; }

            const json fb = fr.value("body", json::object());
            const std::string st = fb.value("status", fr.value("status", ""));
            if (st != "ok") { link_.unregister_stream(csid); continue; }

            total = fb.value("total_size", total);
            const int64_t range_len = fb.value("range_length", 0);
            const uint32_t reply_sid = fb.value("stream_id", csid);
            if (reply_sid != csid) link_.alias_stream(reply_sid, sink);

            const bool done = sink->wait_done(wait_ms_for(range_len));
            std::string chunk;
            { std::lock_guard<std::mutex> lk(sink->m); chunk.swap(sink->out); }
            link_.unregister_stream(csid);
            if (reply_sid != csid) link_.unregister_stream(reply_sid);

            if (!done || static_cast<int64_t>(chunk.size()) < range_len) {
                continue;  // partial/timeout — try the next seeder, same offset
            }
            assembled += chunk;
            if (assembled.size() > max_bytes) { out.error = "too_large"; return out; }
            piece_start += kCountPerFetch;
            out.seeder = seeder;
            got = true;
            break;
        }
        if (!got) { out.error = "fetch_failed"; return out; }
        if (total == 0) break;  // empty file guard
    }

    out.ok    = true;
    out.bytes = std::move(assembled);
    out.mini  = mini;     // the mini that relayed this play's bytes (reward lane)
    return out;
}

} // namespace bopwire::gw
