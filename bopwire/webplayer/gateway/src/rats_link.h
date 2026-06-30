// rats_link.h — the gateway's librats client layer.
//
// The gateway joins the mini-node MESH, not a single relay. It bootstraps to one
// mini, learns the rest via `mininodes.list`, dials them, and load-balances relay
// traffic across them (least-loaded first, one mini pinned per song-play) so no
// single mini's pipes get exhausted as the network grows. This mirrors the native
// player's bestMiniNodePeerId / failover.
//
// Every RPC to a full node or seeder is wrapped in `relay.forward` and sent
// through a chosen mini, which forwards it (stamping the gateway as `originator`)
// and routes the reply back by req_id. Audio bytes arrive as binary frames
// `[stream_id:4 LE][seq:4 LE][eof:1][payload]` (the mini strips the relay
// F-prefix), demultiplexed by stream_id into a StreamSink the streamer waits on.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "librats_c.h"
#include "json.hpp"

namespace bopwire::gw {

using json = nlohmann::json;

// A mini node we can relay through.
struct MiniNode {
    std::string peer_id;
    std::string address;        // "ip:port" from mininodes.list (may be empty for self)
    double      load_score = 1.0;
};

// A full node the mesh knows about (from routes.get).
struct FullNode {
    std::string peer_id;
    std::string public_address;
    std::string reachability;   // "direct" | "relay" | "unknown"
    double      load_score = 1.0;
};

// In-flight RPC awaiting its reply (keyed by req_id).
struct Pending {
    std::mutex              m;
    std::condition_variable cv;
    bool                    done = false;
    std::string            reply;
};

// Reassembles one swarm.fetch byte-range from ordered binary frames.
struct StreamSink {
    std::mutex              m;
    std::condition_variable cv;
    std::map<uint32_t, std::string> buf;   // seq -> payload (handles reorder)
    uint32_t                next = 0;       // next seq to drain in order
    bool                    have_eof = false;
    uint32_t                eof_seq = 0;
    std::string             out;            // assembled, in order
    bool                    done = false;

    void push(uint32_t seq, bool eof, const char* p, size_t n) {
        std::lock_guard<std::mutex> lk(m);
        if (done) return;
        buf[seq] = std::string(p, n);
        if (eof) { have_eof = true; eof_seq = seq; }
        while (true) {
            auto it = buf.find(next);
            if (it == buf.end()) break;
            out += it->second;
            buf.erase(it);
            ++next;
        }
        if (have_eof && next > eof_seq) { done = true; cv.notify_all(); }
    }
    bool wait_done(int timeout_ms) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                           [&] { return done; });
    }
};

class RatsLink {
public:
    RatsLink(std::string bootstrap_host, int bootstrap_port, std::string gateway_id_hex);
    ~RatsLink();

    bool start(int connect_timeout_ms = 20000);
    void stop();

    bool        connected() const { return !bootstrap_pid_.empty(); }
    std::string our_peer_id() const { return gateway_id_; }
    size_t      mini_count();

    // Least-loaded mini we're currently connected to (load-balanced relay hub).
    std::string pick_mini();

    // RPC straight to a mini (routes.get, mininodes.list, …). Auto-picks a mini.
    json rpc_mini(const std::string& type, const json& body, int timeout_ms = 8000);

    // RPC to a full node / seeder, tunnelled via a SPECIFIC mini (pin one per play).
    json rpc_relay(const std::string& mini_peer, const std::string& target_peer_id,
                   const std::string& type, const json& body, int timeout_ms = 15000);
    // Same, auto-picking a mini per call (good for one-shot catalog/session RPCs).
    json rpc_via_relay(const std::string& target_peer_id,
                       const std::string& type, const json& body, int timeout_ms = 15000);

    std::string pick_full_node();
    std::vector<FullNode> full_nodes();

    // Binary stream demux for swarm.fetch.
    uint32_t next_stream_id() { return stream_seq_.fetch_add(1) & 0xFFFFFFFFu; }
    std::shared_ptr<StreamSink> register_stream(uint32_t sid);
    void alias_stream(uint32_t sid, std::shared_ptr<StreamSink> sink);
    void unregister_stream(uint32_t sid);

private:
    static void on_reply_s(void* ud, const char* peer, const char* data);
    static void on_binary_s(void* ud, const char* peer, const void* data, size_t size);
    void on_reply(const char* data);
    void on_binary(const void* data, size_t size);

    json send_and_wait(const std::string& peer_id, json env, int timeout_ms);
    std::string new_req_id();
    bool wait_for_bootstrap(int timeout_ms);
    std::set<std::string> connected_peers();
    void dial(const std::string& address);
    void refresh_minis();           // mininodes.list → grow/dial the mesh
    void refresh_routes();          // routes.get → full-node list
    void mesh_loop();

    const std::string boot_host_;
    const int         boot_port_;
    std::string       gateway_id_;
    rats_client_t     client_ = nullptr;
    std::string       bootstrap_pid_;

    std::atomic<uint32_t> req_seq_{1};
    std::atomic<uint32_t> stream_seq_{1};

    std::mutex pending_mu_;
    std::unordered_map<std::string, std::shared_ptr<Pending>> pending_;

    std::mutex streams_mu_;
    std::unordered_map<uint32_t, std::shared_ptr<StreamSink>> streams_;

    std::mutex minis_mu_;
    std::vector<MiniNode> minis_;

    std::mutex routes_mu_;
    std::vector<FullNode> full_nodes_;

    std::thread       mesh_thread_;
    std::atomic<bool> stopping_{false};
};

} // namespace bopwire::gw
