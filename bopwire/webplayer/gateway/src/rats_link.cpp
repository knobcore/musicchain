// rats_link.cpp — see rats_link.h.
#include "rats_link.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace bopwire::gw {

static constexpr const char* kReqType   = "bopwire.request";
static constexpr const char* kReplyType = "bopwire.reply";

RatsLink::RatsLink(std::string bootstrap_host, int bootstrap_port, std::string gateway_id_hex)
    : boot_host_(std::move(bootstrap_host)), boot_port_(bootstrap_port),
      gateway_id_(std::move(gateway_id_hex)) {}

RatsLink::~RatsLink() { stop(); }

std::string RatsLink::new_req_id() {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "gw-%08x",
                  req_seq_.fetch_add(1, std::memory_order_relaxed));
    return buf;
}

// ───────────────────────── lifecycle ─────────────────────────
bool RatsLink::start(int connect_timeout_ms) {
    client_ = gateway_id_.size() == 40
        ? rats_create_with_id(0, gateway_id_.c_str())
        : rats_create(0);
    if (!client_) { std::fprintf(stderr, "[link] rats_create failed\n"); return false; }
    if (rats_start(client_) != RATS_SUCCESS) {
        std::fprintf(stderr, "[link] rats_start failed\n");
        rats_destroy(client_); client_ = nullptr; return false;
    }
    if (gateway_id_.size() != 40) {
        if (char* pid = rats_get_our_peer_id(client_)) { gateway_id_ = pid; rats_string_free(pid); }
    }
    rats_on_message(client_, kReplyType, &RatsLink::on_reply_s, this);
    rats_set_binary_callback(client_, &RatsLink::on_binary_s, this);

    std::printf("[link] gateway id=%s, bootstrapping via %s:%d\n",
                gateway_id_.c_str(), boot_host_.c_str(), boot_port_);
    (void) rats_connect(client_, boot_host_.c_str(), boot_port_);
    if (!wait_for_bootstrap(connect_timeout_ms)) {
        std::fprintf(stderr, "[link] no bootstrap mini after %dms\n", connect_timeout_ms);
        return false;
    }
    std::printf("[link] bootstrap mini: %s\n", bootstrap_pid_.c_str());
    { std::lock_guard<std::mutex> lk(minis_mu_); minis_.push_back(MiniNode{bootstrap_pid_, "", 0.5}); }
    refresh_minis();
    refresh_routes();
    mesh_thread_ = std::thread(&RatsLink::mesh_loop, this);
    return true;
}

void RatsLink::stop() {
    stopping_ = true;
    if (mesh_thread_.joinable()) mesh_thread_.join();
    if (client_) { rats_stop(client_); rats_destroy(client_); client_ = nullptr; }
}

bool RatsLink::wait_for_bootstrap(int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& pid : connected_peers()) {
            if (!pid.empty()) { bootstrap_pid_ = pid; return true; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

std::set<std::string> RatsLink::connected_peers() {
    std::set<std::string> s;
    int n = 0;
    char** ids = rats_get_validated_peer_ids(client_, &n);
    if (ids) {
        for (int k = 0; k < n; ++k) {
            if (ids[k] && *ids[k]) s.insert(ids[k]);
            rats_string_free(ids[k]);
        }
        std::free(ids);
    }
    return s;
}

void RatsLink::dial(const std::string& address) {
    if (address.empty()) return;
    const size_t colon = address.rfind(':');      // last colon = port (ipv4 or [ipv6])
    if (colon == std::string::npos) return;
    std::string host = address.substr(0, colon);
    const int port = std::atoi(address.substr(colon + 1).c_str());
    if (!host.empty() && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);   // strip ipv6 brackets
    if (port > 0) (void) rats_connect(client_, host.c_str(), port);
}

// ───────────────────────── callbacks ─────────────────────────
void RatsLink::on_reply_s(void* ud, const char*, const char* data) {
    if (ud && data) static_cast<RatsLink*>(ud)->on_reply(data);
}
void RatsLink::on_binary_s(void* ud, const char*, const void* data, size_t size) {
    if (ud && data) static_cast<RatsLink*>(ud)->on_binary(data, size);
}

void RatsLink::on_reply(const char* data) {
    std::string rid;
    try { rid = json::parse(data).value("req_id", ""); } catch (...) { return; }
    if (rid.empty()) return;
    std::shared_ptr<Pending> p;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        auto it = pending_.find(rid);
        if (it == pending_.end()) return;
        p = it->second;
    }
    { std::lock_guard<std::mutex> lk(p->m); p->reply = data; p->done = true; }
    p->cv.notify_all();
}

void RatsLink::on_binary(const void* data, size_t size) {
    if (size < 9) return;
    const auto* b = static_cast<const uint8_t*>(data);
    const uint32_t sid = uint32_t(b[0]) | uint32_t(b[1]) << 8
                       | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
    const uint32_t seq = uint32_t(b[4]) | uint32_t(b[5]) << 8
                       | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
    const bool eof = b[8] != 0;
    std::shared_ptr<StreamSink> sink;
    {
        std::lock_guard<std::mutex> lk(streams_mu_);
        auto it = streams_.find(sid);
        if (it == streams_.end()) return;
        sink = it->second;
    }
    sink->push(seq, eof, reinterpret_cast<const char*>(b + 9), size - 9);
}

// ───────────────────────── RPC core ─────────────────────────
json RatsLink::send_and_wait(const std::string& peer_id, json env, int timeout_ms) {
    const std::string rid = new_req_id();
    env["req_id"] = rid;
    auto p = std::make_shared<Pending>();
    { std::lock_guard<std::mutex> lk(pending_mu_); pending_[rid] = p; }

    const std::string payload = env.dump();
    if (rats_send_message(client_, peer_id.c_str(), kReqType, payload.c_str()) != RATS_SUCCESS) {
        std::lock_guard<std::mutex> lk(pending_mu_); pending_.erase(rid);
        throw std::runtime_error("rats_send_message failed");
    }
    bool ok;
    {
        std::unique_lock<std::mutex> lk(p->m);
        ok = p->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return p->done; });
    }
    std::string reply = ok ? p->reply : std::string();
    { std::lock_guard<std::mutex> lk(pending_mu_); pending_.erase(rid); }
    if (!ok) throw std::runtime_error("rpc timeout for " + env.value("type", std::string("?")));
    return json::parse(reply);
}

json RatsLink::rpc_mini(const std::string& type, const json& body, int timeout_ms) {
    const std::string mini = pick_mini();
    if (mini.empty()) throw std::runtime_error("no mini connected");
    return send_and_wait(mini, json{{"type", type}, {"body", body}}, timeout_ms);
}

json RatsLink::rpc_relay(const std::string& mini_peer, const std::string& target_peer_id,
                         const std::string& type, const json& body, int timeout_ms) {
    if (mini_peer.empty()) throw std::runtime_error("no mini for relay");
    json env = {
        {"type", "relay.forward"},
        {"body", {{"target_peer_id", target_peer_id}, {"type", type}, {"body", body}}},
    };
    return send_and_wait(mini_peer, std::move(env), timeout_ms);
}

json RatsLink::rpc_via_relay(const std::string& target_peer_id,
                             const std::string& type, const json& body, int timeout_ms) {
    return rpc_relay(pick_mini(), target_peer_id, type, body, timeout_ms);
}

// ───────────────────────── mesh ─────────────────────────
size_t RatsLink::mini_count() {
    auto conn = connected_peers();
    std::lock_guard<std::mutex> lk(minis_mu_);
    size_t n = 0;
    for (const auto& m : minis_) if (conn.count(m.peer_id)) ++n;
    return n;
}

std::string RatsLink::pick_mini() {
    auto conn = connected_peers();
    std::lock_guard<std::mutex> lk(minis_mu_);
    const MiniNode* best = nullptr;
    for (const auto& m : minis_)
        if (conn.count(m.peer_id) && (!best || m.load_score < best->load_score)) best = &m;
    if (best) return best->peer_id;
    return bootstrap_pid_;   // last resort
}

void RatsLink::refresh_minis() {
    json r;
    try { r = rpc_mini("mininodes.list", json::object(), 8000); }
    catch (const std::exception& e) { std::fprintf(stderr, "[link] mininodes.list: %s\n", e.what()); return; }

    const json& list = r.value("body", json::array());
    std::vector<MiniNode> found;
    for (const auto& m : list) {
        const std::string pid = m.value("rats_peer_id", "");
        if (pid.empty()) continue;
        found.push_back(MiniNode{pid, m.value("public_address", ""), m.value("load_score", 1.0)});
    }
    // Merge: upsert load/address by peer_id, keep ones we already had.
    {
        std::lock_guard<std::mutex> lk(minis_mu_);
        for (const auto& f : found) {
            auto it = std::find_if(minis_.begin(), minis_.end(),
                                   [&](const MiniNode& x) { return x.peer_id == f.peer_id; });
            if (it == minis_.end()) minis_.push_back(f);
            else { it->load_score = f.load_score; if (!f.address.empty()) it->address = f.address; }
        }
    }
    // Dial any mini we're not yet connected to.
    auto conn = connected_peers();
    std::vector<std::string> to_dial;
    {
        std::lock_guard<std::mutex> lk(minis_mu_);
        for (const auto& m : minis_)
            if (!m.address.empty() && !conn.count(m.peer_id)) to_dial.push_back(m.address);
    }
    for (const auto& a : to_dial) dial(a);
}

void RatsLink::refresh_routes() {
    std::vector<FullNode> out;
    try {
        json r = rpc_mini("routes.get", json::object(), 8000);
        for (const auto& m : r["body"].value("peers", json::array())) {
            const std::string pid = m.value("rats_peer_id", "");
            if (pid.empty()) continue;
            out.push_back(FullNode{pid, m.value("public_address", ""),
                                   m.value("reachability", "unknown"), m.value("load_score", 1.0)});
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[link] routes.get: %s\n", e.what());
        return;   // keep last good list
    }
    std::lock_guard<std::mutex> lk(routes_mu_);
    full_nodes_.swap(out);
}

void RatsLink::mesh_loop() {
    while (!stopping_) {
        for (int i = 0; i < 45 && !stopping_; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (stopping_) break;
        refresh_minis();
        refresh_routes();
    }
}

std::vector<FullNode> RatsLink::full_nodes() {
    std::lock_guard<std::mutex> lk(routes_mu_);
    return full_nodes_;
}

std::string RatsLink::pick_full_node() {
    std::lock_guard<std::mutex> lk(routes_mu_);
    const FullNode* best = nullptr;
    for (const auto& fn : full_nodes_)
        if (!best || fn.load_score < best->load_score) best = &fn;
    return best ? best->peer_id : std::string();
}

// ───────────────────────── stream demux ─────────────────────────
std::shared_ptr<StreamSink> RatsLink::register_stream(uint32_t sid) {
    auto sink = std::make_shared<StreamSink>();
    std::lock_guard<std::mutex> lk(streams_mu_);
    streams_[sid] = sink;
    return sink;
}
void RatsLink::alias_stream(uint32_t sid, std::shared_ptr<StreamSink> sink) {
    std::lock_guard<std::mutex> lk(streams_mu_);
    streams_[sid] = std::move(sink);
}
void RatsLink::unregister_stream(uint32_t sid) {
    std::lock_guard<std::mutex> lk(streams_mu_);
    streams_.erase(sid);
}

} // namespace bopwire::gw
