// library_smoke_cli — minimal native CLI that exercises the same path as
// the player's library tab: connect to VPS, routes.get, then songs.list
// against the full node via relay.forward. Designed to run on Android
// (arm64-v8a) over cellular to validate the live network end-to-end.

#include "librats_c.h"
#include "../deps/librats/src/json.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr const char* kVpsHost   = "85.239.238.226";
constexpr int         kVpsPort   = 8080;
constexpr const char* kReqType   = "bopwire.request";
constexpr const char* kReplyType = "bopwire.reply";

std::mutex              g_mu;
std::condition_variable g_cv;
std::string             g_reply_for;   // req_id we're waiting on
std::string             g_reply_json;  // payload received

void on_reply(void* /*ud*/, const char* /*peer_id*/, const char* data) {
    if (!data) return;
    try {
        auto j = nlohmann::json::parse(data);
        const std::string rid = j.value("req_id", "");
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_reply_for.empty() && rid == g_reply_for) {
            g_reply_json = data;
            g_reply_for.clear();
            g_cv.notify_all();
        }
    } catch (const std::exception&) {}
}

std::string new_req_id() {
    static std::atomic<uint32_t> seq{1};
    char buf[24];
    std::snprintf(buf, sizeof(buf), "cli-%08x",
                  seq.fetch_add(1, std::memory_order_relaxed));
    return buf;
}

bool send_and_wait(rats_client_t client,
                   const std::string& peer_id,
                   const std::string& body_json,
                   nlohmann::json* out,
                   int timeout_ms = 8000) {
    const std::string req_id = new_req_id();
    nlohmann::json env = nlohmann::json::parse(body_json);
    env["req_id"] = req_id;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_reply_for  = req_id;
        g_reply_json.clear();
    }
    if (rats_send_message(client, peer_id.c_str(), kReqType,
                          env.dump().c_str()) != RATS_SUCCESS) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_reply_for.clear();
        std::fprintf(stderr, "[cli] send rc != 0 for req %s\n", req_id.c_str());
        return false;
    }
    std::unique_lock<std::mutex> lk(g_mu);
    if (!g_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                       [] { return g_reply_for.empty(); })) {
        g_reply_for.clear();
        std::fprintf(stderr, "[cli] timeout waiting %dms for reply\n",
                     timeout_ms);
        return false;
    }
    try {
        *out = nlohmann::json::parse(g_reply_json);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[cli] reply parse: %s\n", e.what());
        return false;
    }
    return true;
}

} // namespace

int main() {
    std::printf("=== library_smoke_cli ===\n");
    std::printf("[cli] librats %s\n", rats_get_version_string());

    rats_client_t client = rats_create(0);  // ephemeral port
    if (!client) { std::fprintf(stderr, "rats_create failed\n"); return 1; }
    if (rats_start(client) != 0) {
        std::fprintf(stderr, "rats_start failed\n");
        rats_destroy(client); return 1;
    }

    rats_on_message(client, kReplyType, on_reply, nullptr);

    std::printf("[cli] connecting to VPS %s:%d ...\n", kVpsHost, kVpsPort);
    // rats_connect is async: a non-zero rc here just means the handshake
    // hasn't completed synchronously, not that the connect failed. The real
    // signal is whether the peer shows up in rats_get_validated_peer_ids
    // within the wait window below.
    (void) rats_connect(client, kVpsHost, kVpsPort);

    // Wait for the handshake to land.
    std::string vps_pid;
    for (int i = 0; i < 60; ++i) {
        int n = 0;
        char** ids = rats_get_validated_peer_ids(client, &n);
        if (ids) {
            for (int k = 0; k < n; ++k) {
                if (ids[k] && *ids[k]) { vps_pid = ids[k]; }
                rats_string_free(ids[k]);
            }
            std::free(ids);
        }
        if (!vps_pid.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (vps_pid.empty()) {
        std::fprintf(stderr, "[cli] no VPS peer after 15s\n");
        rats_stop(client); rats_destroy(client); return 3;
    }
    std::printf("[cli] VPS peer: %s\n", vps_pid.c_str());

    // 1) routes.get — directly to VPS
    nlohmann::json routes_reply;
    if (!send_and_wait(client, vps_pid,
                       R"({"type":"routes.get","body":{}})",
                       &routes_reply, 8000)) {
        std::fprintf(stderr, "[cli] routes.get failed\n");
        rats_stop(client); rats_destroy(client); return 4;
    }
    auto peers = routes_reply["body"].value("peers", nlohmann::json::array());
    std::printf("[cli] routes.get -> %zu peers\n", peers.size());

    std::string full_pid;
    for (auto& m : peers) {
        const std::string pid = m.value("rats_peer_id", "");
        if (pid.empty() || pid == vps_pid) continue;
        full_pid = pid;
        std::printf("[cli]   home: %s  pub=%s  reach=%s\n",
                    pid.c_str(),
                    m.value("public_address", "").c_str(),
                    m.value("reachability", "?").c_str());
        break;
    }
    if (full_pid.empty()) {
        std::fprintf(stderr, "[cli] no full node in routes\n");
        rats_stop(client); rats_destroy(client); return 5;
    }

    // 2) songs.list -> full node, wrapped in relay.forward
    nlohmann::json wrap_env = {
        {"type", "relay.forward"},
        {"body", {
            {"target_peer_id", full_pid},
            {"type",           "songs.list"},
            {"body",           nlohmann::json::object()},
        }},
    };
    nlohmann::json songs_reply;
    if (!send_and_wait(client, vps_pid, wrap_env.dump(),
                       &songs_reply, 15000)) {
        std::fprintf(stderr, "[cli] songs.list failed\n");
        rats_stop(client); rats_destroy(client); return 6;
    }
    const auto& body = songs_reply["body"];
    if (body.is_array()) {
        std::printf("[cli] OK: songs.list -> %zu song(s)\n", body.size());
        for (const auto& s : body) {
            std::printf("  - \"%s\"  by %s  album=\"%s\"\n",
                        s.value("title",  "").c_str(),
                        s.value("artist", "").c_str(),
                        s.value("album",  "").c_str());
        }
    } else {
        std::printf("[cli] songs.list reply: %s\n", songs_reply.dump().c_str());
    }

    rats_stop(client);
    rats_destroy(client);
    return 0;
}
