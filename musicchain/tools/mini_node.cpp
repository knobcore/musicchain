/**
 * mini_node.cpp — MusicChain VPS rendezvous node.
 *
 * Pure librats — no HTTP, no DHT layer. Every full node and every player
 * connects to this binary via librats UDP. Full nodes publish their routing
 * record on the `musicchain.routes` topic every 15 minutes; players ask for
 * the routing table with the librats RPC verb `routes.get`.
 *
 * Usage:
 *   mini-node [--rats-port N] [--quiet]
 */

// mc_rats_quic.h was the old transparent shim. We now link the real
// librats (deps/librats) compiled with a QUIC backend, so consume its C
// bindings header instead.
#include "librats_c.h"
#include "../src/net/load_monitor.h"
#include "../src/crypto/keys.h"
#include "../src/crypto/bip39.h"   // bip39_generate_12 / bip39_mnemonic_to_keypair
#include <fstream>
#include <memory>
#include <sys/stat.h>

// librats vendors nlohmann::json at src/json.hpp; reuse it here so we can
// parse incoming RPC envelopes properly (the previous ad-hoc string-search
// parser ran into trouble once librats's own serializer started sorting
// keys alphabetically — `body` came before `type`, so a search for the
// first `"type":"` returned the inner verb name instead of the outer
// `relay.forward`).
#include "json.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr uint16_t kDefaultRatsPort = 8080; // librats project default (plain TCP)
constexpr const char* kRoutesTopic   = "musicchain.routes";
constexpr const char* kRequestType   = "musicchain.request";
constexpr const char* kReplyType     = "musicchain.reply";

// Mini-node mesh: every mini-node sends `mini.hello` to every fresh peer
// to discover whether the other side is also a mini-node. Mini-nodes that
// respond OK get tracked in g_mininode_peers and receive replication of
// every route we ingest so a full node connected to VPS A is visible from
// VPS B's routes.get reply (load-balancing for players).
constexpr const char* kMiniHelloType = "mini.hello";

// ---- Relay state ----------------------------------------------------
//
// When a player can't reach a full node directly (NAT, ISP block) it wraps
// each RPC as `relay.forward` to the mini-node. The mini-node forwards the
// inner envelope to the target node on its existing QUIC link, swapping in
// a fresh req_id so the target's reply can be matched back to the originator.

struct PendingRelay {
    std::string originator_peer_id;
    std::string original_req_id;
    uint64_t    created_ms;
};

std::mutex                                       g_relay_mu;
std::unordered_map<std::string, PendingRelay>    g_pending_relays;

std::string new_relay_req_id() {
    static std::atomic<uint64_t> counter{1};
    char buf[40];
    std::snprintf(buf, sizeof(buf), "relay-%016llx",
                  (unsigned long long)counter.fetch_add(1));
    return buf;
}

// Reachability is the result of the mini-node opening a fresh QUIC connection
// back to a node's STUN-discovered public_address from a different ephemeral
// source port. "direct" means anyone behind any other NAT can punch through;
// "relay" means the node's NAT only lets the originally-connected mini-node
// flow in, so peer-to-peer traffic has to be relayed via this binary.
//
// "unknown" is the initial state until the test completes.
enum class Reachability : uint8_t { Unknown, Direct, Relay };

inline const char* reachability_str(Reachability r) {
    switch (r) {
        case Reachability::Direct: return "direct";
        case Reachability::Relay:  return "relay";
        default:                   return "unknown";
    }
}

struct RouteEntry {
    std::string  node_id;
    std::string  rats_peer_id;
    std::string  public_address;
    int          api_port = 0;
    uint64_t     ts       = 0;
    uint64_t     received_at_ms = 0;
    Reachability reachability   = Reachability::Unknown;
    uint64_t     reachability_tested_at_ms = 0;
    // Load fields parsed from the route message. Players use these to
    // pick the lightest peer when multiple full nodes are reachable.
    // Default to zero (= unknown / idle) so a peer that never reports
    // doesn't get artificially boosted.
    float        load_score = 0.0f;
    float        cpu_load   = 0.0f;
    uint64_t     net_bps    = 0;
    bool         is_busy    = false;
};

std::atomic<bool>  g_running{true};
std::mutex         g_routes_mu;
std::unordered_map<std::string, RouteEntry> g_routes;
bool               g_quiet = false;

// Peers that responded to mini.hello are tracked here so we can replicate
// every fresh route to them. The "from_mininode" loop guard in ingest_route
// prevents an A→B→A→B forwarding ping-pong.
std::mutex                                    g_mininodes_mu;
std::unordered_set<std::string>               g_mininode_peers;
// Public address of each mini-node peer (best-effort, from rats_get_peer_info_json
// when we recorded them). Used by mininodes.list so a player can directly dial
// any of them. Stored separately from g_mininode_peers so the loop guard stays
// O(1) lookup.
std::unordered_map<std::string, std::string>  g_mininode_addr;

// CLI-seeded list of mini-nodes to dial at startup. Lets a fresh VPS bootstrap
// onto an existing mesh by knowing one already-running peer.
std::vector<std::string> g_seed_mininodes;

// Player registry. Players announce themselves with `player.announce` so
// that other players downloading songs can resolve a swarm member's
// public_address before attempting a direct rats_connect (the ICE-equivalent
// hole-punch path). The address is the NAT-mapped (ip:port) we observed
// when the player connected to us — see ingest_player_announce.
struct PlayerEntry {
    std::string  rats_peer_id;
    std::string  public_address; // ip:port observed by the VPS
    uint64_t     announced_at_ms = 0;
};
std::mutex                                       g_players_mu;
std::unordered_map<std::string, PlayerEntry>     g_players;

// ---- Chat module: IRC-like channels over librats gossipsub ----------
//
// Two topic conventions used here:
//
//   "chat:rooms"          — global directory. Anyone publishes a JSON
//                           room announcement when they create a room;
//                           every mini-node that hears it auto-
//                           subscribes to that room's per-room topic.
//
//   "chat:room:<name>"    — per-room message stream. Players publish
//                           signed JSON messages; mini-nodes persist
//                           the last `kChatRingPerRoom` in memory so a
//                           player joining mid-conversation can fetch
//                           recent history via the chat.history verb.
//
// Multi-mini-node sync + load balancing fall straight out of
// gossipsub's mesh protocol — we don't write any explicit replication
// code. A new mini-node joining the mesh starts receiving messages on
// every topic it subscribes to within a few seconds, and the bounded-
// degree mesh distributes load automatically.

constexpr const char* kChatRoomsTopic  = "chat:rooms";
constexpr const char* kChatRoomPrefix  = "chat:room:";
constexpr size_t      kChatRingPerRoom = 1000;

struct ChatRoom {
    std::string name;
    std::string topic_str;   // human-readable description
    std::string creator;     // 0x-prefixed address that announced it
    uint64_t    created_ms = 0;
    bool        is_private = false;
};

struct ChatMessage {
    std::string from;        // 0x-prefixed address
    std::string from_pubkey; // 33-byte compressed pubkey hex, for sig recovery
    uint64_t    ts_ms = 0;
    std::string body;        // plaintext for public rooms, ECIES hex for private
    std::string sig;         // ECDSA over canonical JSON of (from,ts_ms,body,room)
};

std::mutex                                              g_chat_rooms_mu;
std::unordered_map<std::string, ChatRoom>               g_chat_rooms;

std::mutex                                              g_chat_msgs_mu;
std::unordered_map<std::string, std::deque<ChatMessage>> g_chat_msgs;

// Forward declarations; bodies appear below the rats_client_t globals.
void chat_subscribe_room(rats_client_t client, const std::string& name);
void on_chat_room_announce(void*, const char*, const char*, const char*);
void on_chat_message(void*, const char*, const char*, const char*);

// ---- Event log (TUI monitor) ----------------------------------------
//
// The monitor renders a fixed-height view of recent peer events. The ring
// buffer is sized to one console window so there's no scrollback to flood;
// when an event arrives that would push past the window, the oldest event is
// dropped.

// 22 rows for the event list + 2 header rows + 2 footer rows = 26-line screen.
constexpr size_t kEventRows = 22;

struct EventEntry {
    uint64_t    ts_ms;
    std::string kind;
    std::string peer_id;
    std::string detail;
};

std::mutex             g_events_mu;
std::deque<EventEntry> g_events;
std::atomic<int>       g_peer_count{0};

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

void on_signal(int) { g_running = false; }

void push_event(std::string kind, std::string peer_id, std::string detail) {
    EventEntry e;
    e.ts_ms   = now_ms();
    e.kind    = std::move(kind);
    e.peer_id = std::move(peer_id);
    e.detail  = std::move(detail);
    std::lock_guard<std::mutex> lk(g_events_mu);
    g_events.emplace_back(std::move(e));
    while (g_events.size() > kEventRows) g_events.pop_front();
}

std::string fmt_hms(uint64_t ts_ms) {
    const std::time_t t = static_cast<std::time_t>(ts_ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string fmt_short_peer(const std::string& peer_id) {
    if (peer_id.size() <= 12) return peer_id;
    return peer_id.substr(0, 12) + "...";
}

void redraw_monitor() {
    // ANSI: clear screen + cursor home. Works on Windows 10+ conhost,
    // Windows Terminal, and every Linux terminal.
    std::ostringstream out;
    out << "\033[2J\033[H";
    out << "\033[1m musicchain mini-node \033[0m"
        << "  peers=" << g_peer_count.load();
    {
        std::lock_guard<std::mutex> lk(g_routes_mu);
        out << "  routes=" << g_routes.size();
    }
    out << "  (Ctrl+C to quit)\n";
    out << "----------------------------------------------------------------------\n";
    out << " time      event       peer/route                                     \n";
    out << "----------------------------------------------------------------------\n";

    std::deque<EventEntry> snapshot;
    {
        std::lock_guard<std::mutex> lk(g_events_mu);
        snapshot = g_events;
    }

    // Print most recent at the bottom so it reads chronologically. Pad with
    // blank lines so the screen height is constant and there is no scroll.
    const size_t blanks = (snapshot.size() < kEventRows)
                              ? (kEventRows - snapshot.size()) : 0;
    for (size_t i = 0; i < blanks; ++i) out << "\n";
    for (const auto& e : snapshot) {
        std::string kind_padded = e.kind;
        if (kind_padded.size() < 10) kind_padded.append(10 - kind_padded.size(), ' ');
        const std::string pid = fmt_short_peer(e.peer_id);
        out << " " << fmt_hms(e.ts_ms) << "  "
            << kind_padded << "  " << pid;
        if (!e.detail.empty()) out << "  " << e.detail;
        out << "\n";
    }
    out << "----------------------------------------------------------------------\n";
    std::cout << out.str() << std::flush;
}

// ---- tiny JSON parsing for the topic messages -----------------------
//
// The route JSON is fixed-shape — we only need to pluck four fields.

std::string json_get_string(const std::string& src, const std::string& key) {
    const std::string needle = "\"" + key + "\":\"";
    auto p = src.find(needle);
    if (p == std::string::npos) return {};
    p += needle.size();
    auto q = src.find('"', p);
    if (q == std::string::npos) return {};
    return src.substr(p, q - p);
}

uint64_t json_get_uint(const std::string& src, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    auto p = src.find(needle);
    if (p == std::string::npos) return 0;
    p += needle.size();
    while (p < src.size() && (src[p] == ' ' || src[p] == '\t')) ++p;
    uint64_t v = 0;
    while (p < src.size() && src[p] >= '0' && src[p] <= '9') {
        v = v * 10 + (src[p] - '0');
        ++p;
    }
    return v;
}

// Mini-node's own LoadMonitor — same metric stack as the full node so
// players see one consistent score across both peer types. Heap-
// allocated because LoadMonitor owns a std::mutex + std::thread, which
// rule out reassignment. main() builds it once with the resolved cfg.
std::unique_ptr<mc::net::LoadMonitor> g_load_mon;

// Mini-node's wallet identity — same BIP39 + BIP32 derivation the user
// wallet uses. Address is published in routes.get so full nodes know
// where to send the RelayRewardTx credits. main() loads or generates
// it before the rats client starts.
std::string g_wallet_address_hex;  // EIP-55 checksummed
std::string g_wallet_address_raw;  // 40-char lowercase hex

std::string routes_json() {
    std::ostringstream ss;
    mc::net::LoadMonitor::Snapshot self_load{};
    if (g_load_mon) self_load = g_load_mon->current();
    ss << "{\"self_load\":{"
       << "\"load_score\":"  << self_load.load_score          << ","
       << "\"cpu_load\":"    << self_load.cpu_load            << ","
       << "\"net_bps\":"     << self_load.net_bytes_per_sec   << ","
       << "\"is_busy\":"     << (self_load.is_busy ? "true" : "false")
       << "},\"wallet\":\""  << g_wallet_address_hex
       << "\",\"peers\":[";
    bool first = true;
    std::lock_guard<std::mutex> lk(g_routes_mu);
    for (const auto& kv : g_routes) {
        const auto& r = kv.second;
        if (!first) ss << ",";
        first = false;
        // Strip the STUN port off public_address ("1.2.3.4:5678" → "1.2.3.4")
        // so we can pair the IP with the node's HTTP api_port.
        std::string ipv4;
        const auto colon = r.public_address.find(':');
        if (colon != std::string::npos) ipv4 = r.public_address.substr(0, colon);
        else                            ipv4 = r.public_address;

        std::string api_url;
        if (!ipv4.empty() && r.api_port > 0)
            api_url = "http://" + ipv4 + ":" + std::to_string(r.api_port);

        ss << "{"
           << "\"node_id\":\""        << r.node_id        << "\","
           << "\"rats_peer_id\":\""   << r.rats_peer_id   << "\","
           << "\"ipv6\":\"\","
           << "\"ipv4\":\""           << ipv4             << "\","
           << "\"api_port\":"         << r.api_port      << ","
           << "\"api_url\":\""        << api_url          << "\","
           << "\"public_address\":\"" << r.public_address << "\","
           << "\"reachability\":\""   << reachability_str(r.reachability) << "\","
           << "\"load_score\":"       << r.load_score    << ","
           << "\"cpu_load\":"         << r.cpu_load      << ","
           << "\"net_bps\":"          << r.net_bps       << ","
           << "\"is_busy\":"          << (r.is_busy ? "true" : "false") << ","
           << "\"last_seen_ms\":"     << r.received_at_ms
           << "}";
    }
    ss << "]}";
    return ss.str();
}

// ---- Reachability probe ---------------------------------------------
//
// For each route we receive, spawn a one-shot worker that brings up a fresh
// mc_rats_quic client on an OS-picked ephemeral port and tries to connect to
// the node's STUN-discovered public_address. If the QUIC handshake completes
// within `kProbeTimeoutMs`, the node's NAT lets independent flows in (full-
// cone or restricted-cone-with-permissive-mapping) → reachability = direct.
// If the handshake times out, peer-to-peer traffic must be relayed through
// this mini-node instead → reachability = relay.

constexpr int kProbeTimeoutMs = 3000;

void mark_reachability(const std::string& node_id, Reachability r) {
    std::lock_guard<std::mutex> lk(g_routes_mu);
    auto it = g_routes.find(node_id);
    if (it == g_routes.end()) return;
    it->second.reachability             = r;
    it->second.reachability_tested_at_ms = now_ms();
    if (!g_quiet) {
        push_event("reach", it->second.node_id, reachability_str(r));
    }
}

void probe_reachability(std::string node_id, std::string public_address) {
    std::thread([node_id = std::move(node_id),
                 public_address = std::move(public_address)]() mutable {
        if (public_address.empty()) return;
        const auto colon = public_address.find(':');
        if (colon == std::string::npos) return;
        const std::string host = public_address.substr(0, colon);
        int port;
        try { port = std::stoi(public_address.substr(colon + 1)); }
        catch (...) { return; }

        rats_client_t probe = rats_create(0); // ephemeral port
        if (!probe) { mark_reachability(node_id, Reachability::Unknown); return; }

        std::atomic<bool> handshook{false};
        rats_set_connection_callback(probe, [](void* ud, const char* peer_id) {
            *static_cast<std::atomic<bool>*>(ud) = true;
            if (peer_id) rats_string_free(peer_id);
        }, &handshook);

        if (rats_start(probe) != 0) {
            rats_destroy(probe);
            mark_reachability(node_id, Reachability::Unknown);
            return;
        }
        rats_connect(probe, host.c_str(), port);

        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(kProbeTimeoutMs);
        while (!handshook.load() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        mark_reachability(node_id,
            handshook.load() ? Reachability::Direct : Reachability::Relay);

        rats_destroy(probe);
    }).detach();
}

// ---- librats callbacks ----------------------------------------------

void ingest_route(const std::string& body, const char* peer_id);

// Mini-node mesh helpers — defined further down once g_client is in scope.
// Declared here because ingest_route uses them to fan a freshly-received
// route out to every other mini-node we know about.
void send_mini_hello(const std::string& peer_id);
void replicate_routes_to_peer(const std::string& peer_id);
void replicate_route_to_mininodes(const std::string& route_json,
                                  const std::string& source_peer_id);
bool peer_is_mininode(const std::string& peer_id);
void broadcast_swarm_peer_offline(const std::string& disconnected_pid);
void broadcast_swarm_peer_online(const std::string& online_pid);
void replay_online_players_to_home(const std::string& full_pid);

// Re-test reachability if the last probe is older than this.
constexpr uint64_t kProbeMinIntervalMs = 60'000;

void ingest_route(const std::string& body, const char* peer_id) {
    RouteEntry e;
    e.node_id        = json_get_string(body, "node_id");
    e.rats_peer_id   = json_get_string(body, "rats_peer_id");
    e.public_address = json_get_string(body, "public_address");
    e.api_port       = static_cast<int>(json_get_uint(body, "api_port"));
    e.ts             = json_get_uint(body, "ts");
    e.received_at_ms = now_ms();
    // Load fields — present in routes from the new full-node build.
    // Parse via nlohmann::json so floats and bools work, the
    // hand-rolled json_get_string used above is string-only.
    try {
        auto j = nlohmann::json::parse(body);
        if (j.contains("load_score") && j["load_score"].is_number())
            e.load_score = j["load_score"].get<float>();
        if (j.contains("cpu_load")   && j["cpu_load"].is_number())
            e.cpu_load = j["cpu_load"].get<float>();
        if (j.contains("net_bps")    && j["net_bps"].is_number())
            e.net_bps = j["net_bps"].get<uint64_t>();
        if (j.contains("is_busy")    && j["is_busy"].is_boolean())
            e.is_busy = j["is_busy"].get<bool>();
    } catch (...) { /* older format — leave defaults */ }
    // If the route message didn't carry an explicit rats_peer_id, the
    // sender's transport peer id is the next best thing.
    if (e.rats_peer_id.empty() && peer_id) e.rats_peer_id = peer_id;

    if (e.node_id.empty()) return;

    bool need_probe = false;
    {
        std::lock_guard<std::mutex> lk(g_routes_mu);
        auto it = g_routes.find(e.node_id);
        if (it == g_routes.end()) {
            need_probe = true;
        } else {
            // Preserve last probe result + only re-probe if stale.
            e.reachability             = it->second.reachability;
            e.reachability_tested_at_ms = it->second.reachability_tested_at_ms;
            const uint64_t age = e.received_at_ms - it->second.reachability_tested_at_ms;
            if (it->second.reachability == Reachability::Unknown ||
                age > kProbeMinIntervalMs ||
                e.public_address != it->second.public_address) {
                need_probe = true;
            }
        }
        g_routes[e.node_id] = e;
    }
    if (g_quiet) {
        std::cout << "[routes] " << e.node_id.substr(0, 12) << "... "
                  << e.public_address << " :" << e.api_port
                  << " (via " << (peer_id ? peer_id : "?") << ")\n";
    } else {
        std::string detail = e.public_address + ":" + std::to_string(e.api_port);
        push_event("route", e.node_id, detail);
    }
    if (need_probe && !e.public_address.empty()) {
        probe_reachability(e.node_id, e.public_address);
    }

    // Replicate to other mini-nodes UNLESS the sender is itself a
    // mini-node — that means we got this route as a forward and
    // re-forwarding would loop.
    const std::string source = peer_id ? peer_id : "";
    if (!peer_is_mininode(source)) {
        replicate_route_to_mininodes(body, source);
    }

    // Full node just (re)published its route to us, which means it
    // either reconnected or is brand new on the mesh. Replay every
    // currently-online player so its swarm view recovers without
    // waiting up to 5 minutes for each player's next swarm.hello tick.
    // Only fires for direct sources — relayed routes (from another
    // mini-node) don't have the full node directly available to us.
    if (!source.empty() && !peer_is_mininode(source)) {
        if (!e.rats_peer_id.empty()) {
            replay_online_players_to_home(e.rats_peer_id);
        }
    }
}

void on_route_message(void* /*ud*/, const char* peer_id, const char* /*topic*/,
                      const char* message) {
    if (!message) return;
    ingest_route(message, peer_id);
}

void on_route_direct(void* /*ud*/, const char* peer_id, const char* message_data) {
    if (message_data) ingest_route(message_data, peer_id);
    // librats_c.cpp strdup's both args; this synchronous consumer frees.
    if (peer_id)      rats_string_free(peer_id);
    if (message_data) rats_string_free(message_data);
}

void on_peer_connected(void* /*ud*/, const char* peer_id) {
    g_peer_count.fetch_add(1);
    if (g_quiet) {
        std::cout << "[rats] peer connected: " << (peer_id ? peer_id : "?") << "\n";
    } else {
        push_event("connect", peer_id ? peer_id : "?", "");
    }
    // Probe the new peer: if they're another mini-node, our mini.hello
    // gets a positive ack and they get tracked for route replication.
    // Players and full nodes silently ignore (unknown_type), which we
    // treat as "not a mini-node". The 500 ms delay gives librats's noise
    // / handshake state time to settle so the send doesn't race the
    // negotiation.
    std::string pid = peer_id ? peer_id : "";
    if (!pid.empty()) {
        std::thread([pid]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            send_mini_hello(pid);
        }).detach();
    }
}

void on_peer_disconnected(void* /*ud*/, const char* peer_id) {
    int prev = g_peer_count.fetch_sub(1);
    if (prev <= 0) g_peer_count.store(0);
    if (g_quiet) {
        std::cout << "[rats] peer disconnected: " << (peer_id ? peer_id : "?") << "\n";
    } else {
        push_event("disconnect", peer_id ? peer_id : "?", "");
    }
    bool was_mininode = false;
    if (peer_id) {
        std::lock_guard<std::mutex> lk(g_mininodes_mu);
        was_mininode = g_mininode_peers.erase(peer_id) > 0;
        g_mininode_addr.erase(peer_id);
    }
    // Drop the player record so the next route-publish from a home
    // node doesn't replay an offline peer back as "online".
    if (peer_id && !was_mininode) {
        std::lock_guard<std::mutex> lk(g_players_mu);
        g_players.erase(peer_id);
    }
    // Non-mini-node disconnects need to propagate to the full nodes —
    // they otherwise have no signal that a relayed player went offline
    // and would keep that player's songs visible. Skip mini-node peers
    // (the mesh handles those via direct rats reconnection and mini.hello).
    if (peer_id && *peer_id && !was_mininode) {
        broadcast_swarm_peer_offline(peer_id);
    }
}

// ---- librats RPC handler --------------------------------------------
//
// Players ask us for the current routing table with:
//   {req_id, type:"routes.get", body:{}}
// We answer on `musicchain.reply` with the same JSON list we used to serve
// over the HTTP /dht-peers endpoint.

rats_client_t g_client = nullptr;

void send_reply(const char* peer_id, const std::string& reply_json) {
    if (!g_client || !peer_id) return;
    rats_send_message(g_client, peer_id, kReplyType, reply_json.c_str());
}

// ---- Mini-node mesh helpers -----------------------------------------

void send_mini_hello(const std::string& peer_id) {
    if (!g_client || peer_id.empty()) return;
    nlohmann::json req = {
        {"req_id", new_relay_req_id()},
        {"type",   kMiniHelloType},
        {"body",   nlohmann::json::object()},
    };
    rats_send_message(g_client, peer_id.c_str(), kRequestType,
                      req.dump().c_str());
}

// Push every route currently in g_routes to a single peer (called when a
// fresh mini-node joins the mesh so its players see our full nodes right
// away instead of waiting for the next 5-minute republish).
void replicate_routes_to_peer(const std::string& peer_id) {
    if (!g_client || peer_id.empty()) return;
    std::vector<RouteEntry> snapshot;
    {
        std::lock_guard<std::mutex> lk(g_routes_mu);
        snapshot.reserve(g_routes.size());
        for (const auto& kv : g_routes) snapshot.push_back(kv.second);
    }
    for (const auto& e : snapshot) {
        nlohmann::json r = {
            {"node_id",        e.node_id},
            {"rats_peer_id",   e.rats_peer_id},
            {"public_address", e.public_address},
            {"api_port",       e.api_port},
            {"ts",             e.ts},
        };
        rats_send_message(g_client, peer_id.c_str(), kRoutesTopic,
                          r.dump().c_str());
    }
}

// When we ingest a route from a non-mini-node peer (i.e., a fresh
// publish straight from a full node), forward it to every known
// mini-node peer so their players see this full node too. The
// from_mininode guard below breaks A→B→A→B loops.
void replicate_route_to_mininodes(const std::string& route_json,
                                  const std::string& source_peer_id) {
    if (!g_client) return;
    std::vector<std::string> peers;
    {
        std::lock_guard<std::mutex> lk(g_mininodes_mu);
        peers.reserve(g_mininode_peers.size());
        for (const auto& p : g_mininode_peers) {
            if (p == source_peer_id) continue; // don't echo back
            peers.push_back(p);
        }
    }
    for (const auto& p : peers) {
        rats_send_message(g_client, p.c_str(), kRoutesTopic,
                          route_json.c_str());
    }
}

bool peer_is_mininode(const std::string& peer_id) {
    if (peer_id.empty()) return false;
    std::lock_guard<std::mutex> lk(g_mininodes_mu);
    return g_mininode_peers.count(peer_id) > 0;
}

void broadcast_swarm_peer_offline(const std::string& disconnected_pid) {
    if (!g_client || disconnected_pid.empty()) return;
    int count = 0;
    char** ids = rats_get_validated_peer_ids(g_client, &count);
    if (!ids) return;
    // Build the request envelope once — every recipient sees the same
    // payload, only the destination differs.
    nlohmann::json env = {
        {"req_id", new_relay_req_id()},
        {"type",   "swarm.peer_offline"},
        {"body",   {{"peer_id", disconnected_pid}}},
    };
    const std::string payload = env.dump();

    // Snapshot the mini-node set under the lock so we can iterate
    // recipients without holding it during the send loop.
    std::unordered_set<std::string> mininodes;
    {
        std::lock_guard<std::mutex> lk(g_mininodes_mu);
        mininodes = g_mininode_peers;
    }

    int sent = 0;
    for (int i = 0; i < count; ++i) {
        if (!ids[i]) continue;
        const std::string pid(ids[i]);
        rats_string_free(ids[i]);
        if (pid == disconnected_pid) continue; // unreachable anyway
        if (mininodes.count(pid) > 0) continue; // mesh handles those
        rats_send_message(g_client, pid.c_str(),
                          kRequestType, payload.c_str());
        ++sent;
    }
    std::free(ids);
    if (sent > 0 && g_quiet) {
        std::cout << "[mini-node] swarm.peer_offline "
                  << disconnected_pid.substr(0, 12)
                  << " -> " << sent << " full node(s)\n";
    } else if (!g_quiet && sent > 0) {
        push_event("swarm-off", disconnected_pid,
                   std::to_string(sent) + " full node(s)");
    }
}

// Broadcast a single online event for `online_pid` to every connected
// full node (any validated peer that isn't a mini-node). Symmetric with
// broadcast_swarm_peer_offline; called from player.announce so a
// fresh player connection is immediately visible to every full node
// without waiting for the player to re-send swarm.hello.
void broadcast_swarm_peer_online(const std::string& online_pid) {
    if (!g_client || online_pid.empty()) return;
    int count = 0;
    char** ids = rats_get_validated_peer_ids(g_client, &count);
    if (!ids) return;
    nlohmann::json env = {
        {"req_id", new_relay_req_id()},
        {"type",   "swarm.peer_online"},
        {"body",   {{"peer_id", online_pid}}},
    };
    const std::string payload = env.dump();
    std::unordered_set<std::string> mininodes;
    {
        std::lock_guard<std::mutex> lk(g_mininodes_mu);
        mininodes = g_mininode_peers;
    }
    int sent = 0;
    for (int i = 0; i < count; ++i) {
        if (!ids[i]) continue;
        const std::string pid(ids[i]);
        rats_string_free(ids[i]);
        if (pid == online_pid) continue;
        if (mininodes.count(pid) > 0) continue;
        rats_send_message(g_client, pid.c_str(),
                          kRequestType, payload.c_str());
        ++sent;
    }
    std::free(ids);
    if (sent > 0 && !g_quiet) {
        push_event("swarm-on", online_pid,
                   std::to_string(sent) + " full node(s)");
    }
}

// Replay every player we have a g_players record for to a single home
// node (called when the full node just published its route — typically
// either first contact or a restart). Lets the full node's swarm
// connection-state catch up without each player having to retick
// their syncSwarm path.
void replay_online_players_to_home(const std::string& full_pid) {
    if (!g_client || full_pid.empty()) return;
    std::vector<std::string> players;
    {
        std::lock_guard<std::mutex> lk(g_players_mu);
        players.reserve(g_players.size());
        for (const auto& [pid, _] : g_players) players.push_back(pid);
    }
    int sent = 0;
    for (const auto& pid : players) {
        if (pid == full_pid) continue;
        nlohmann::json env = {
            {"req_id", new_relay_req_id()},
            {"type",   "swarm.peer_online"},
            {"body",   {{"peer_id", pid}}},
        };
        rats_send_message(g_client, full_pid.c_str(),
                          kRequestType, env.dump().c_str());
        ++sent;
    }
    if (sent > 0 && !g_quiet) {
        push_event("swarm-replay", full_pid,
                   std::to_string(sent) + " player(s)");
    }
}

std::string peer_address(const char* peer_id) {
    if (!g_client || !peer_id) return {};
    char* info = rats_get_peer_info_json(g_client, peer_id);
    if (!info) return {};
    std::string addr;
    try {
        auto j = nlohmann::json::parse(info);
        const std::string ip = j.value("ip", std::string());
        const int port = j.value("source_port", j.value("port", 0));
        if (!ip.empty() && port > 0) addr = ip + ":" + std::to_string(port);
    } catch (const nlohmann::json::exception&) {}
    rats_string_free(info);
    return addr;
}

// ---- Chat callbacks (bodies) ---------------------------------------

void chat_subscribe_room(rats_client_t client, const std::string& name) {
    const std::string topic = std::string(kChatRoomPrefix) + name;
    if (rats_is_subscribed_to_topic(client, topic.c_str())) return;
    rats_subscribe_to_topic(client, topic.c_str());
    rats_set_topic_message_callback(client, topic.c_str(),
                                    on_chat_message, nullptr);
}

void on_chat_room_announce(void* /*ud*/, const char* /*peer_id*/,
                            const char* /*topic*/, const char* msg) {
    if (!msg) return;
    try {
        auto j = nlohmann::json::parse(msg);
        ChatRoom room;
        room.name       = j.value("name",       std::string());
        if (room.name.empty()) return;
        room.topic_str  = j.value("topic",      std::string());
        room.creator    = j.value("creator",    std::string());
        room.created_ms = j.value("created_ms", static_cast<uint64_t>(0));
        room.is_private = j.value("private",    false);
        bool added = false;
        {
            std::lock_guard<std::mutex> lk(g_chat_rooms_mu);
            added = g_chat_rooms.emplace(room.name, room).second;
        }
        if (added) {
            chat_subscribe_room(g_client, room.name);
            if (!g_quiet) {
                std::cerr << "[mini-node] chat-room registered: "
                          << room.name
                          << (room.is_private ? " (private)" : "")
                          << "\n";
            }
        }
    } catch (const nlohmann::json::exception&) {
        // Malformed announcement — drop.
    }
}

void on_chat_message(void* /*ud*/, const char* /*peer_id*/,
                     const char* topic, const char* msg) {
    if (!msg || !topic) return;
    std::string topic_str = topic;
    if (topic_str.compare(0, std::strlen(kChatRoomPrefix), kChatRoomPrefix) != 0) {
        return;
    }
    std::string room_name = topic_str.substr(std::strlen(kChatRoomPrefix));
    try {
        auto j = nlohmann::json::parse(msg);
        ChatMessage m;
        m.from        = j.value("from",        std::string());
        m.from_pubkey = j.value("from_pubkey", std::string());
        m.ts_ms       = j.value("ts_ms",       static_cast<uint64_t>(0));
        m.body        = j.value("body",        std::string());
        m.sig         = j.value("sig",         std::string());
        if (m.from.empty() || m.ts_ms == 0) return;
        std::lock_guard<std::mutex> lk(g_chat_msgs_mu);
        auto& dq = g_chat_msgs[room_name];
        dq.push_back(std::move(m));
        while (dq.size() > kChatRingPerRoom) dq.pop_front();
    } catch (const nlohmann::json::exception&) {
        // Malformed message — drop.
    }
}

void on_rpc_request(void* /*ud*/, const char* peer_id, const char* message_data) {
    // librats_c.cpp strdup's both args; free at every return path.
    if (!peer_id || !message_data) {
        if (peer_id) rats_string_free(peer_id);
        if (message_data) rats_string_free(message_data);
        return;
    }

    nlohmann::json env;
    try {
        env = nlohmann::json::parse(message_data);
    } catch (const nlohmann::json::exception&) {
        rats_string_free(peer_id);
        rats_string_free(message_data);
        return;
    }
    const std::string req_id = env.value("req_id", std::string());
    const std::string type   = env.value("type",   std::string());
    if (req_id.empty() || type.empty()) {
        rats_string_free(peer_id);
        rats_string_free(message_data);
        return;
    }

    std::cerr << "[mini-node] on_rpc_request type=" << type
              << " from " << (peer_id ? std::string(peer_id).substr(0,12) : "(null)")
              << " req_id=" << req_id.substr(0, std::min<size_t>(req_id.size(), 16))
              << std::endl;

    std::ostringstream r;
    if (type == "routes.get") {
        // routes_json() already returns {"peers":[...]} — embed it as `body`.
        r << "{\"req_id\":\"" << req_id << "\",\"status\":\"ok\",\"body\":"
          << routes_json() << "}";
    } else if (type == kMiniHelloType) {
        // The sender is identifying themselves as a mini-node. Register
        // them, capture their NAT-mapped address for mininodes.list, and
        // push our current route table over so their players see our
        // full nodes immediately. The send_mini_hello call below makes
        // the discovery bidirectional in case THEY connected to US (and
        // so our on_peer_connected hasn't fired in the same direction).
        const std::string pid = peer_id;
        const std::string addr = peer_address(peer_id);
        bool added = false;
        {
            std::lock_guard<std::mutex> lk(g_mininodes_mu);
            added = g_mininode_peers.insert(pid).second;
            if (!addr.empty()) g_mininode_addr[pid] = addr;
        }
        if (added && !g_quiet) {
            push_event("vps-peer", pid, addr);
        }
        if (added) {
            // First time we've heard from this mini-node — kick off a
            // route snapshot so their cache catches up to ours.
            const std::string pid_copy = pid;
            std::thread([pid_copy]() {
                replicate_routes_to_peer(pid_copy);
            }).detach();
            // And echo a mini.hello so the other side adds us too in
            // case they connected first.
            send_mini_hello(pid);
        }
        nlohmann::json body{{"role",         "mini-node"},
                            {"peer_address", addr},
                            {"route_count",  static_cast<uint64_t>(g_routes.size())}};
        r << "{\"req_id\":\"" << req_id << "\",\"status\":\"ok\",\"body\":"
          << body.dump() << "}";
    } else if (type == "mininodes.list") {
        // Return everyone we've identified as a peer mini-node, plus
        // ourselves. The player merges these into its own bootstrap
        // list so it can fail over to a different mini-node without
        // any operator intervention.
        nlohmann::json arr = nlohmann::json::array();
        {
            char* own = rats_get_our_peer_id(g_client);
            if (own) {
                arr.push_back({{"rats_peer_id", own},
                               {"public_address", std::string()},
                               {"self", true}});
                rats_string_free(own);
            }
        }
        {
            std::lock_guard<std::mutex> lk(g_mininodes_mu);
            for (const auto& p : g_mininode_peers) {
                auto it = g_mininode_addr.find(p);
                arr.push_back({
                    {"rats_peer_id",   p},
                    {"public_address", it == g_mininode_addr.end()
                                           ? std::string()
                                           : it->second},
                    {"self", false},
                });
            }
        }
        r << "{\"req_id\":\"" << req_id << "\",\"status\":\"ok\",\"body\":"
          << arr.dump() << "}";
    } else if (type == "chat.list_rooms") {
        // Returns every room this mini-node has heard about via the
        // chat:rooms gossipsub topic. Players call it on launch and
        // every time the "Social" tab is opened.
        nlohmann::json arr = nlohmann::json::array();
        {
            std::lock_guard<std::mutex> lk(g_chat_rooms_mu);
            for (const auto& [_, room] : g_chat_rooms) {
                arr.push_back({
                    {"name",       room.name},
                    {"topic",      room.topic_str},
                    {"creator",    room.creator},
                    {"created_ms", room.created_ms},
                    {"private",    room.is_private},
                });
            }
        }
        r << "{\"req_id\":\"" << req_id << "\",\"status\":\"ok\",\"body\":"
          << arr.dump() << "}";
    } else if (type == "chat.history") {
        // Returns up to `limit` recent messages for `room`, optionally
        // before `before_ts_ms` (for paged scroll-back). Body shape:
        //   {"room":"#general","limit":50,"before_ts_ms":1718596800000}
        auto body = env.value("body", nlohmann::json::object());
        const std::string room = body.value("room", std::string());
        const uint64_t before  = body.value("before_ts_ms",
                                            static_cast<uint64_t>(0));
        int limit              = body.value("limit", 50);
        if (limit < 1)   limit = 1;
        if (limit > 200) limit = 200;
        nlohmann::json arr = nlohmann::json::array();
        if (!room.empty()) {
            std::lock_guard<std::mutex> lk(g_chat_msgs_mu);
            auto it = g_chat_msgs.find(room);
            if (it != g_chat_msgs.end()) {
                int taken = 0;
                for (auto rit = it->second.rbegin();
                     rit != it->second.rend() && taken < limit;
                     ++rit) {
                    if (before > 0 && rit->ts_ms >= before) continue;
                    arr.push_back({
                        {"from",        rit->from},
                        {"from_pubkey", rit->from_pubkey},
                        {"ts_ms",       rit->ts_ms},
                        {"body",        rit->body},
                        {"sig",         rit->sig},
                    });
                    ++taken;
                }
            }
        }
        r << "{\"req_id\":\"" << req_id << "\",\"status\":\"ok\",\"body\":"
          << arr.dump() << "}";
    } else if (type == "stun.observe") {
        // STUN-equivalent over our librats-on-QUIC link: the caller asks
        // "what address do you see me from?", we look it up in our local
        // peer table and echo back. No external STUN protocol involved —
        // it's just JSON over the same QUIC stream the caller is already
        // using, so we don't need any extra ports beyond UDP/443.
        //
        // Critically we use `source_port` (the actual NAT-mapped external
        // port the connection came from), not `port` — librats's handshake
        // overwrites `port` with the peer's claimed listen_port, which
        // is the wrong number for telling them their reachable address.
        std::string addr;
        char* info = rats_get_peer_info_json(g_client, peer_id);
        if (info) {
            try {
                auto j = nlohmann::json::parse(info);
                const std::string ip = j.value("ip", std::string());
                const int port = j.value("source_port", j.value("port", 0));
                if (!ip.empty() && port > 0) addr = ip + ":" + std::to_string(port);
            } catch (const nlohmann::json::exception&) {}
            rats_string_free(info);
        }
        nlohmann::json body{{"observed_address", addr}};
        r << "{\"req_id\":\"" << req_id
          << "\",\"status\":\"ok\",\"body\":" << body.dump() << "}";
    } else if (type == "player.announce") {
        // Player tells us "I am peer X, register me so others can find me".
        // The public_address we record is the address WE see them from,
        // not whatever they claim — that's what other peers would punch
        // toward when trying a direct rats_connect.
        std::string addr;
        char* info = rats_get_peer_info_json(g_client, peer_id);
        if (info) {
            try {
                auto j = nlohmann::json::parse(info);
                const std::string ip = j.value("ip", std::string());
                const int port = j.value("source_port", j.value("port", 0));
                if (!ip.empty() && port > 0) addr = ip + ":" + std::to_string(port);
            } catch (const nlohmann::json::exception&) {}
            rats_string_free(info);
        }
        bool first_seen = false;
        {
            std::lock_guard<std::mutex> lk(g_players_mu);
            auto [it, inserted] = g_players.try_emplace(peer_id);
            it->second.rats_peer_id    = peer_id;
            it->second.public_address  = addr;
            it->second.announced_at_ms = now_ms();
            first_seen = inserted;
        }
        // First-seen player → broadcast online to every full node so
        // their songs become visible in Discover the moment the
        // player's app comes up, without waiting for the next
        // swarm.hello tick to ripple through.
        if (first_seen) {
            broadcast_swarm_peer_online(std::string(peer_id));
        }
        nlohmann::json body{{"public_address", addr},
                            {"player_count",   static_cast<uint64_t>(
                                                   g_players.size())}};
        r << "{\"req_id\":\"" << req_id
          << "\",\"status\":\"ok\",\"body\":" << body.dump() << "}";
    } else if (type == "player.locate") {
        // Given a peer_id (single or list), return whatever we know about
        // each player's public address. Used by other players before they
        // attempt a direct rats_connect to a swarm member.
        const auto& inner = env.value("body", nlohmann::json::object());
        nlohmann::json out = nlohmann::json::array();
        std::vector<std::string> targets;
        if (inner.contains("peer_id") && inner["peer_id"].is_string()) {
            targets.push_back(inner["peer_id"].get<std::string>());
        } else if (inner.contains("peer_ids") && inner["peer_ids"].is_array()) {
            for (const auto& v : inner["peer_ids"]) {
                if (v.is_string()) targets.push_back(v.get<std::string>());
            }
        }
        std::lock_guard<std::mutex> lk(g_players_mu);
        for (const auto& pid : targets) {
            auto it = g_players.find(pid);
            if (it == g_players.end()) {
                out.push_back({{"peer_id", pid},
                               {"known",   false}});
            } else {
                out.push_back({
                    {"peer_id",        pid},
                    {"known",          true},
                    {"public_address", it->second.public_address},
                    {"announced_at_ms", it->second.announced_at_ms},
                });
            }
        }
        r << "{\"req_id\":\"" << req_id
          << "\",\"status\":\"ok\",\"body\":" << out.dump() << "}";
    } else if (type == "status") {
        size_t routes_n;
        {
            std::lock_guard<std::mutex> lk(g_routes_mu);
            routes_n = g_routes.size();
        }
        r << "{\"req_id\":\"" << req_id << "\",\"status\":\"ok\","
          << "\"body\":{\"role\":\"mini-node\",\"routes\":" << routes_n
          << ",\"peers\":" << g_peer_count.load() << "}}";
    } else if (type == "ice.connect_request") {
        // Body: {target_pid, my_addr: "ip:port"}
        // Player asks us to invite full_node (target_pid) to hole-punch back
        // to its public address. We forward the invite as an ice.connect_invite
        // request to target_pid via our existing peer connection to it; the
        // full node responds by initiating a hole-punch outbound to my_addr.
        const auto& inner = env.value("body", nlohmann::json::object());
        const std::string target  = inner.value("target_pid", std::string());
        const std::string my_addr = inner.value("my_addr",    std::string());
        if (target.empty() || my_addr.empty()) {
            r << "{\"req_id\":\"" << req_id << "\",\"status\":\"invalid_ice\","
              << "\"error\":\"ice.connect_request needs target_pid and my_addr\"}";
            send_reply(peer_id, r.str());
            rats_string_free(peer_id);
            rats_string_free(message_data);
            return;
        }
        nlohmann::json invite = {
            {"req_id", new_relay_req_id()},
            {"type",   "ice.connect_invite"},
            {"body",   {
                {"caller_addr", my_addr},
                {"caller_pid",  std::string(peer_id)},
            }},
        };
        auto rc = rats_send_message(g_client, target.c_str(), kRequestType,
                                      invite.dump().c_str());
        if (!g_quiet) {
            push_event("ice-invite", target,
                       "punch to " + my_addr + " requested by "
                                  + std::string(peer_id).substr(0, 12));
        }
        r << "{\"req_id\":\"" << req_id << "\",\"status\":\""
          << (rc == RATS_SUCCESS ? "ok" : "send_failed")
          << "\",\"body\":{\"forwarded_to\":\"" << target << "\"}}";
    } else if (type == "relay.push.forward") {
        // Fire-and-forget push from a relayed node back to one of our
        // other connected peers. Body: {target_peer_id, type, body}.
        // The mini-node wraps it as a normal musicchain.reply with the
        // inner type set verbatim, so the receiver sees it on its push
        // channel (no req_id correlation needed).
        const auto& inner = env.value("body", nlohmann::json::object());
        const std::string target     = inner.value("target_peer_id", std::string());
        const std::string inner_type = inner.value("type",           std::string());
        const auto&       inner_body = inner.value("body",           nlohmann::json::object());
        std::cout << "[mini-node] relay.push.forward from "
                  << std::string(peer_id).substr(0, 12)
                  << " -> " << target.substr(0, 12)
                  << " type=" << inner_type << "\n";
        std::string status = "ok";
        std::string err;
        if (target.empty() || inner_type.empty()) {
            status = "invalid_push";
            err = "relay.push.forward needs target_peer_id and type";
        } else {
            nlohmann::json push = {
                {"req_id", new_relay_req_id()},
                {"type",   inner_type},
                {"body",   inner_body},
            };
            auto rc = rats_send_message(g_client, target.c_str(),
                                          kReplyType, push.dump().c_str());
            std::cout << "[mini-node]   push rc=" << (int)rc
                      << " bytes=" << push.dump().size() << "\n";
            if (rc != RATS_SUCCESS) {
                status = "send_failed";
                err = "rats_send_message rc=" + std::to_string((int)rc);
            }
            if (!g_quiet) {
                push_event("relay-push", target,
                           inner_type + " from " + std::string(peer_id).substr(0, 12));
            }
        }
        r << "{\"req_id\":\"" << req_id << "\",\"status\":\"" << status
          << "\",\"body\":{\"error\":\"" << err << "\"}}";
    } else if (type == "relay.forward") {
        // Body: {target_peer_id, type, body}
        // Extract the inner envelope and forward it on our QUIC link to the
        // target node. The target's reply is caught by on_relay_reply()
        // below and routed back to the original requester.
        const auto& inner = env.value("body", nlohmann::json::object());
        const std::string target     = inner.value("target_peer_id", std::string());
        const std::string inner_type = inner.value("type",           std::string());
        const std::string inner_body = inner.contains("body")
                                        ? inner["body"].dump()
                                        : std::string("{}");

        if (target.empty() || inner_type.empty()) {
            r << "{\"req_id\":\"" << req_id
              << "\",\"status\":\"invalid_relay\","
              << "\"error\":\"relay.forward needs target_peer_id and type\"}";
            send_reply(peer_id, r.str());
            rats_string_free(peer_id);
            rats_string_free(message_data);
            return;
        }

        // Store the mapping so we can route the eventual reply back.
        const std::string fresh = new_relay_req_id();
        {
            std::lock_guard<std::mutex> lk(g_relay_mu);
            g_pending_relays[fresh] = PendingRelay{
                std::string(peer_id), req_id, now_ms()
            };
        }
        const std::string originator_pid = peer_id ? std::string(peer_id)
                                                   : std::string();
        nlohmann::json fwd = {
            {"req_id", fresh},
            {"type",   inner_type},
            {"body",   nlohmann::json::parse(inner_body)},
            // The originator field lets the target (full node) push later
            // notifications (e.g. upload.complete) back to the original
            // caller by wrapping them in relay.push.forward — without
            // this hint the full node only knows about the VPS as its
            // immediate peer.
            {"originator_peer_id", originator_pid},
        };
        std::cerr << "[mini-node] relay.forward "
                  << inner_type << " from \""
                  << originator_pid
                  << "\" (len=" << originator_pid.size() << ")"
                  << " -> " << target.substr(0, 12)
                  << "  fwd_bytes=" << fwd.dump().size()
                  << "  has_orig_in_fwd="
                  << (fwd.contains("originator_peer_id") ? "yes" : "no")
                  << std::endl;
        if (!g_quiet) {
            push_event("relay-fwd", target,
                       inner_type + " from " + originator_pid.substr(0, 12));
        }
        rats_send_message(g_client, target.c_str(), kRequestType, fwd.dump().c_str());
        rats_string_free(peer_id);
        rats_string_free(message_data);
        return; // mini-node sends no immediate reply — wait for the relayed answer
    } else {
        r << "{\"req_id\":\"" << req_id
          << "\",\"status\":\"unknown_type\","
          << "\"error\":\"mini-node only serves routes.get/mininodes.list/mini.hello/status/stun.observe/relay.forward/relay.push.forward/ice.connect_request\"}";
    }

    send_reply(peer_id, r.str());
    rats_string_free(peer_id);
    rats_string_free(message_data);
}

// ---- Relay reply interceptor ----------------------------------------
//
// Catches every `musicchain.reply` directed at the mini-node, checks whether
// the req_id is one we minted while forwarding, and if so routes the reply
// back to the original requester with the original req_id substituted in.

void on_relay_reply(void* /*ud*/, const char* peer_id, const char* message_data) {
    // librats_c.cpp strdup's both args; free at every return path.
    if (!peer_id || !message_data) {
        if (peer_id) rats_string_free(peer_id);
        if (message_data) rats_string_free(message_data);
        return;
    }
    nlohmann::json env;
    try {
        env = nlohmann::json::parse(message_data);
    } catch (const nlohmann::json::exception&) {
        rats_string_free(peer_id);
        rats_string_free(message_data);
        return;
    }
    const std::string req_id = env.value("req_id", std::string());

    PendingRelay rec;
    bool matched = false;
    {
        std::lock_guard<std::mutex> lk(g_relay_mu);
        auto it = g_pending_relays.find(req_id);
        if (it != g_pending_relays.end()) {
            rec = it->second;
            g_pending_relays.erase(it);
            matched = true;
        }
    }
    if (!matched) {
        rats_string_free(peer_id);
        rats_string_free(message_data);
        return;
    }

    env["req_id"] = rec.original_req_id;
    rats_send_message(g_client, rec.originator_peer_id.c_str(),
                      kReplyType, env.dump().c_str());
    if (!g_quiet) {
        push_event("relay-rep", rec.originator_peer_id,
                   "via " + std::string(peer_id).substr(0, 12));
    }
    rats_string_free(peer_id);
    rats_string_free(message_data);
}

// ---- Binary relay --------------------------------------------------
//
// Cellular peers behind symmetric NAT cannot complete ICE hole-punch, so
// even their *binary* traffic (upload chunks, stream chunks) has to take
// the relay route through us. We mirror what relay.forward does for JSON
// RPCs, but the in-band envelope lives in the binary payload itself —
// JSON+base64 would cost ~33 % bandwidth on music files, so we use a
// small tag instead. Wire format:
//
//   byte 0      : 'F' (relay-binary-forward marker, 0x46)
//   bytes 1..20 : target peer_id as 20 raw bytes (the librats peer_id is
//                 40 hex chars; we accept either the hex form encoded
//                 as ASCII or the raw 20-byte form — we test by length:
//                 if the next 40 bytes are all hex ASCII, treat as hex;
//                 otherwise raw bytes. To keep things simple we *only*
//                 accept the 40-byte hex ASCII form here, since Dart
//                 already has the peer_id as a hex string.)
//   bytes 41..N : payload (handed verbatim to rats_send_binary on target)
//
// Receiver-side (full node, player, etc.) sees the inner payload via its
// regular binary callback — no awareness of the relay needed.

constexpr uint8_t kRelayBinaryTag = 'F';

void on_relay_binary(void* /*ud*/, const char* peer_id,
                     const void* data, size_t size) {
    // librats_c.cpp strdup/malloc's both args; free at every return path.
    // The binary buffer was allocated with malloc() in librats_c.cpp, so
    // rats_string_free (which is just free()) is the correct allocator.
    auto cleanup = [&]() {
        if (peer_id) rats_string_free(peer_id);
        if (data)    rats_string_free(static_cast<const char*>(data));
    };
    if (!peer_id || !data) { cleanup(); return; }
    const uint8_t* b = static_cast<const uint8_t*>(data);
    if (size < 1 + 40 || b[0] != kRelayBinaryTag) { cleanup(); return; }
    char target_hex[41];
    std::memcpy(target_hex, b + 1, 40);
    target_hex[40] = '\0';
    for (int i = 0; i < 40; ++i) {
        const char c = target_hex[i];
        const bool is_hex = (c >= '0' && c <= '9')
                         || (c >= 'a' && c <= 'f')
                         || (c >= 'A' && c <= 'F');
        if (!is_hex) { cleanup(); return; }
    }
    const size_t payload_size = size - 1 - 40;
    if (!g_quiet) {
        push_event("relay-bin", std::string(target_hex),
                   std::to_string(payload_size) + " B from "
                       + std::string(peer_id).substr(0, 12));
    }
    rats_send_binary(g_client, target_hex,
                      b + 1 + 40, payload_size);
    cleanup();
}

} // namespace

int main(int argc, char** argv) {
    uint16_t rats_port = kDefaultRatsPort;
    std::string config_path;
    mc::net::LoadConfig load_cfg;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--rats-port" && i + 1 < argc) {
            rats_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (a == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (a == "--max-bps" && i + 1 < argc) {
            load_cfg.max_bandwidth_bps =
                static_cast<uint64_t>(std::atoll(argv[++i]));
        } else if (a == "--busy-threshold" && i + 1 < argc) {
            load_cfg.busy_score_threshold =
                static_cast<float>(std::atof(argv[++i]));
        } else if (a == "--quiet") {
            g_quiet = true;
        } else if (a == "--peer-vps" && i + 1 < argc) {
            // Comma-separated host:port list. Each one is dialed once at
            // startup so a fresh mini-node can bootstrap onto an existing
            // mesh by knowing any one already-running peer.
            std::string list = argv[++i];
            std::string cur;
            for (char c : list) {
                if (c == ',') {
                    if (!cur.empty()) g_seed_mininodes.push_back(cur);
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
            if (!cur.empty()) g_seed_mininodes.push_back(cur);
        } else if (a == "-h" || a == "--help") {
            std::cout << "Usage: mini-node [--rats-port N] [--quiet]\n"
                      << "  --rats-port  librats TCP port (default " << kDefaultRatsPort << ")\n"
                      << "  --quiet      suppress per-peer log lines\n"
                      << "  --peer-vps   host:port[,host:port...] of fellow mini-nodes\n"
                      << "               to dial at startup so we mesh with them\n";
            return 0;
        } else {
            // legacy --http-port flag is silently accepted+ignored so existing
            // systemd unit files keep working until they get re-deployed.
            if (a == "--http-port" && i + 1 < argc) { ++i; continue; }
            std::cerr << "unknown argument: " << a << "\n";
            return 1;
        }
    }

    // Load persisted config so a re-run of mini-node with no flags
    // reuses the same load-monitor settings + rats port. Config path
    // defaults to ./mini-node.json next to the binary; --config wins.
    {
        if (config_path.empty()) {
            const std::string probe = "./mini-node.json";
            std::ifstream tf(probe);
            if (tf) config_path = probe;
        }
        if (!config_path.empty()) {
            std::ifstream f(config_path);
            if (f) {
                try {
                    nlohmann::json j; f >> j;
                    if (j.contains("rats_port") &&
                        rats_port == kDefaultRatsPort)
                        rats_port = j["rats_port"].get<uint16_t>();
                    if (j.contains("load_monitor")) {
                        const auto& lm = j["load_monitor"];
                        if (lm.contains("max_bandwidth_bps"))
                            load_cfg.max_bandwidth_bps = lm["max_bandwidth_bps"];
                        if (lm.contains("cpu_weight"))
                            load_cfg.cpu_weight = lm["cpu_weight"];
                        if (lm.contains("net_weight"))
                            load_cfg.net_weight = lm["net_weight"];
                        if (lm.contains("sample_interval_ms"))
                            load_cfg.sample_interval_ms = lm["sample_interval_ms"];
                        if (lm.contains("busy_score_threshold"))
                            load_cfg.busy_score_threshold = lm["busy_score_threshold"];
                        if (lm.contains("disable_net_metric"))
                            load_cfg.disable_net_metric = lm["disable_net_metric"];
                        if (lm.contains("disable_cpu_metric"))
                            load_cfg.disable_cpu_metric = lm["disable_cpu_metric"];
                    }
                } catch (...) { /* keep CLI / default values */ }
            }
        }
        // Save back so any CLI overrides land on disk too.
        std::string save_path = config_path.empty()
            ? std::string("./mini-node.json") : config_path;
        try {
            nlohmann::json j;
            j["rats_port"] = rats_port;
            nlohmann::json lm;
            lm["max_bandwidth_bps"]    = load_cfg.max_bandwidth_bps;
            lm["cpu_weight"]           = load_cfg.cpu_weight;
            lm["net_weight"]           = load_cfg.net_weight;
            lm["sample_interval_ms"]   = load_cfg.sample_interval_ms;
            lm["busy_score_threshold"] = load_cfg.busy_score_threshold;
            lm["disable_net_metric"]   = load_cfg.disable_net_metric;
            lm["disable_cpu_metric"]   = load_cfg.disable_cpu_metric;
            j["load_monitor"] = lm;
            std::ofstream of(save_path);
            of << j.dump(2);
        } catch (...) {}
    }
    g_load_mon = std::make_unique<mc::net::LoadMonitor>(load_cfg);
    g_load_mon->start();

    // ---- Wallet identity ---------------------------------------------
    //
    // First-launch: generate a fresh 12-word BIP39 mnemonic and write it
    // to mini-node.seed in the working directory (or the operator-
    // supplied path via $MUSICCHAIN_MINI_SEED). Re-launch: load whatever
    // is already there. Either way we derive the secp256k1 keypair via
    // the same path the user wallets use (m/44'/19779'/0'/0/0) so the
    // resulting address is portable — an operator can import the
    // mnemonic into MetaMask, ethers.js, the player's wallet flow, etc.
    // and see the same address.
    {
        std::string seed_path = "mini-node.seed";
        if (const char* env = std::getenv("MUSICCHAIN_MINI_SEED")) {
            if (*env) seed_path = env;
        }
        std::string mnemonic;
        {
            std::ifstream f(seed_path);
            if (f) std::getline(f, mnemonic);
        }
        while (!mnemonic.empty() &&
               (mnemonic.back() == '\r' || mnemonic.back() == '\n' ||
                mnemonic.back() == ' ')) {
            mnemonic.pop_back();
        }
        if (mnemonic.empty()) {
            mnemonic = mc::crypto::bip39_generate_12();
            if (mnemonic.empty()) {
                std::cerr << "[mini-node] FATAL: entropy source failed\n";
                return 5;
            }
            std::ofstream out(seed_path, std::ios::trunc);
            if (!out) {
                std::cerr << "[mini-node] FATAL: cannot write "
                          << seed_path << " — refusing to lose seed\n";
                return 5;
            }
            out << mnemonic << "\n";
            std::cout << "[mini-node] generated new wallet seed at "
                      << seed_path << "\n";
            std::cout << "[mini-node] FIRST-LAUNCH MNEMONIC: " << mnemonic
                      << "\n";
            std::cout << "[mini-node] WRITE THIS DOWN. After this run it's "
                         "only on disk at the path above. Reward MC tokens "
                         "earned via tunneled traffic go to this address.\n";
        }
        auto kp_opt = mc::crypto::bip39_mnemonic_to_keypair(mnemonic, "");
        if (!kp_opt) {
            std::cerr << "[mini-node] FATAL: BIP32 derivation failed\n";
            return 5;
        }
        g_wallet_address_hex = mc::crypto::to_checksum_hex(kp_opt->address);
        std::ostringstream raw;
        for (uint8_t b : kp_opt->address) {
            raw << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(b);
        }
        g_wallet_address_raw = raw.str();
        std::cout << "[mini-node] wallet address: " << g_wallet_address_hex
                  << "\n";
    }

    // In TUI mode librats' own console logger would scribble over our redraws,
    // so disable it. In --quiet mode (systemd) we let librats log to stdout.
    rats_set_console_logging_enabled(g_quiet ? 1 : 0);
    rats_set_log_level(g_quiet ? "INFO" : "WARN");

    rats_client_t client = rats_create(rats_port);
    if (!client) {
        std::cerr << "[mini-node] rats_create failed on port " << rats_port << "\n";
        return 2;
    }
    g_client = client;
    rats_set_max_peers(client, 4096);

    if (rats_start(client) != 0) {
        std::cerr << "[mini-node] rats_start failed\n";
        rats_destroy(client);
        return 3;
    }

    rats_set_connection_callback(client, on_peer_connected,    nullptr);
    rats_set_disconnect_callback(client, on_peer_disconnected, nullptr);

    rats_subscribe_to_topic(client, kRoutesTopic);
    rats_set_topic_message_callback(client, kRoutesTopic, on_route_message, nullptr);

    // Chat: subscribe to the global room-announcement topic. When a
    // player publishes a room creation here, on_chat_room_announce
    // adds the room to our directory AND auto-subscribes to the
    // per-room messages topic. Multi-mini-node sync comes for free
    // via the gossipsub mesh.
    rats_subscribe_to_topic(client, kChatRoomsTopic);
    rats_set_topic_message_callback(client, kChatRoomsTopic,
                                    on_chat_room_announce, nullptr);

    // Direct typed-message receiver for route broadcasts from full nodes (the
    // path the full node uses today, since gossipsub mesh form-up is too slow
    // for a first publish).
    rats_on_message(client, kRoutesTopic, on_route_direct, nullptr);

    // RPC handler so players can pull the routing table over librats
    // (replaces the old HTTP /api/v1/net/dht-peers endpoint).
    rats_on_message(client, kRequestType, on_rpc_request, nullptr);

    // Catch reply envelopes from forwarded relay traffic and route them back
    // to the original requester.
    rats_on_message(client, kReplyType, on_relay_reply, nullptr);

    // Binary relay — for cellular peers behind symmetric NAT whose direct
    // hole-punch attempts fail. See on_relay_binary above.
    rats_set_binary_callback(client, on_relay_binary, nullptr);

    rats_start_automatic_peer_discovery(client);

    // Dial every seed mini-node. on_peer_connected → send_mini_hello
    // promotes them to g_mininode_peers as soon as the handshake settles,
    // and from there route gossip flows both directions.
    for (const auto& hp : g_seed_mininodes) {
        const auto colon = hp.rfind(':');
        if (colon == std::string::npos) {
            std::cerr << "[mini-node] --peer-vps entry missing port: "
                      << hp << "\n";
            continue;
        }
        const std::string host = hp.substr(0, colon);
        int port = 0;
        try { port = std::stoi(hp.substr(colon + 1)); }
        catch (...) {
            std::cerr << "[mini-node] --peer-vps bad port: " << hp << "\n";
            continue;
        }
        if (rats_connect(client, host.c_str(), port) != 0) {
            std::cerr << "[mini-node] seed dial pending: " << hp
                      << " (librats async-connect)\n";
        } else {
            std::cout << "[mini-node] seed dial: " << hp << "\n";
        }
    }

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    char* pid = rats_get_our_peer_id(client);
    std::cout << "[mini-node] rats listening on port " << rats_port
              << " (peer id: " << (pid ? pid : "?") << ")\n";
    if (pid) rats_string_free(pid);

    if (g_quiet) {
        std::cout << "[mini-node] ready - full nodes and players may connect.\n";
    } else {
        push_event("startup", "", "ready, awaiting peers");
        redraw_monitor();
    }

    // In TUI mode we redraw the monitor every second so the timestamp footer
    // and peer count stay live even when nothing is happening. In --quiet mode
    // we emit a one-line status every minute (clean for journald).
    auto next_redraw = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    auto next_report = std::chrono::steady_clock::now() + std::chrono::minutes(1);
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const auto now = std::chrono::steady_clock::now();

        // Keep the displayed peer count honest (the callbacks can race).
        g_peer_count.store(rats_get_peer_count(client));

        if (!g_quiet && now >= next_redraw) {
            redraw_monitor();
            next_redraw = now + std::chrono::seconds(1);
        }
        if (g_quiet && now >= next_report) {
            size_t routes_n;
            {
                std::lock_guard<std::mutex> lk(g_routes_mu);
                routes_n = g_routes.size();
            }
            std::cout << "[mini-node] peers=" << g_peer_count.load()
                      << " routes=" << routes_n << "\n";
            next_report = now + std::chrono::minutes(1);
        }
    }

    std::cout << "[mini-node] shutting down...\n";
    rats_stop_automatic_peer_discovery(client);
    rats_stop(client);
    rats_destroy(client);
    g_client = nullptr;
    return 0;
}
