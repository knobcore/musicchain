/**
 * mini_node.cpp — Bopwire VPS rendezvous node.
 *
 * Pure librats — no HTTP, no DHT layer. Every full node and every player
 * connects to this binary via librats UDP. Full nodes publish their routing
 * record on the `bopwire.routes` topic every 15 minutes; players ask for
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
#include "../src/crypto/hash.h"       // from_hex / to_hex for chat sig verify
#include "../src/crypto/signature.h"  // sign_data for relay.report (#10), verify_data for chat
#include "../src/crypto/bip39.h"   // bip39_generate_12 / bip39_mnemonic_to_keypair
#include <array>                    // DeliveryAccum delivery_id (#10)
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

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>   // sender-thread work queue (#6)
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>           // std::function tasks for the sender queue (#6)
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>                // sender-thread work queue (#6)
#include <random>               // seed new_relay_req_id from a random base (#4)
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr uint16_t kDefaultRatsPort = 8080; // librats project default (plain TCP)
constexpr const char* kRoutesTopic   = "bopwire.routes";
constexpr const char* kRequestType   = "bopwire.request";
constexpr const char* kReplyType     = "bopwire.reply";

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
    uint64_t    created_ms;        // monotonic stamp (#8) for the TTL sweep
    // (#4 reply correlation) the peer we forwarded TO. on_relay_reply requires
    // the reply's transport sender == this before matching, so a stray reply
    // from some other peer carrying a guessed/collided req_id can't hijack the
    // originator's channel.
    std::string target_peer_id;
};

std::mutex                                       g_relay_mu;
std::unordered_map<std::string, PendingRelay>    g_pending_relays;
// (#1 instability fix) g_pending_relays must be reaped: an entry leaks for
// every relay.forward whose target never replies (full-node restart, dropped
// link, fire-and-forget inner verb, player abandoning its request). Nothing
// matched it before, so a busy VPS grew it without bound → slow OOM + rising
// g_relay_mu hash-probe cost mesh-wide.
//
// The TTL is intentionally tight: 9s sits just past the player's 8s stream
// timeout (so an in-flight chunk reply is never dropped early) yet well under
// its 15s RPC timeout, so the synthetic fail-fast `relay_timeout` reply lands
// BEFORE the player's own timer trips. The sweep that uses it runs on a 1s
// cadence (pending_sweep_loop), decoupled from the 30s route reaper, so the
// reply isn't stuck behind the slower route-cleanup tick. created_ms is a
// monotonic stamp (#8) — see the relay.forward handler.
constexpr uint64_t kPendingRelayTtlMs = 9'000;
// Cadence of the dedicated pending-relay sweeper. 1s keeps the fail-fast reply
// punctual relative to kPendingRelayTtlMs without busy-spinning the CPU.
constexpr int      kPendingSweepIntervalMs = 1'000;
std::atomic<bool>  g_pending_sweep_running{false};
std::thread        g_pending_sweep_thread;

std::string new_relay_req_id() {
    // (#4) Seed from a random 64-bit base instead of 1 so req_ids minted after
    // a restart don't collide with ids the peer mesh saw from a prior process
    // (a reused id could let a late reply for an old forward match a brand-new
    // pending entry). std::random_device seeds a 64-bit splitmix-style mixer
    // once; the per-call increment keeps ids unique within this process.
    static std::atomic<uint64_t> counter{[]() -> uint64_t {
        std::random_device rd;
        uint64_t hi = rd(), lo = rd();
        return (hi << 32) ^ lo ^ 0x9E3779B97F4A7C15ULL;
    }()};
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
    // Verbatim signed envelope {route,pubkey,sig} this route arrived in, or
    // empty for a legacy unsigned route. Kept so catch-up replication to a
    // freshly-joined mini forwards a route the peer can independently verify,
    // and so the replay guard knows this entry came from a signed source.
    std::string  signed_envelope;
};

std::atomic<bool>  g_running{true};
std::mutex         g_routes_mu;
std::unordered_map<std::string, RouteEntry> g_routes;
bool               g_quiet = false;

// ---- Route TTL + reaper ---------------------------------------------
//
// Audit found g_routes had no cleanup, so a full node that vanished
// (crashed, lost its VPS link, migrated) would linger here forever and
// keep getting served to players in routes.get. The reaper thread wakes
// every kReaperIntervalMs and erases any RouteEntry whose received_at_ms
// is older than kRouteTtlMs. Mirror-cleans g_mininode_load /
// g_mininode_addr so the per-peer load reply doesn't reference a
// dead route.
constexpr uint64_t kRouteTtlMs       = 10 * 60 * 1000; // 10 min
constexpr int      kReaperIntervalMs = 30 * 1000;      // 30 s
std::atomic<bool>  g_reaper_running{false};
std::thread        g_reaper_thread;

// ---- Per-peer binary-relay rate limit -------------------------------
//
// Each connected peer gets a token bucket sized to kRelayBucketBytes
// (50 MB), refilled at kRelayRefillBps (10 MB/s). Tokens are charged
// per inbound 'F'-tagged relay frame; if a peer's bucket goes negative
// we drop the frame instead of forwarding so one chatty cellular peer
// can't saturate the VPS uplink for everyone. Buckets for peers that
// no longer appear in rats_get_validated_peer_ids are pruned by the
// reaper above.
struct RelayBucket {
    uint64_t tokens         = 0;  // current credit (bytes)
    uint64_t last_refill_ms = 0;
};
constexpr uint64_t kRelayBucketBytes = 50ULL * 1024 * 1024; // 50 MB burst
constexpr uint64_t kRelayRefillBps   = 10ULL * 1024 * 1024; // 10 MB/s sustained
std::mutex                                       g_relay_bucket_mu;
std::unordered_map<std::string, RelayBucket>     g_relay_buckets;

// #10 relay-reward triangulation: per-delivery_id byte accumulator. On each
// relayed F-frame we add the payload bytes; the reaper flushes a signed
// relay.report on idle timeout. We DON'T learn the broker from the F-frame
// (its target is the originator player, not the broker that minted the
// delivery_id), so the reaper broadcasts the report to every full node in
// g_routes — only the broker holds the matching pd: row; the rest reply
// "ignored". Keyed by lowercase-hex of the 16-byte delivery_id.
struct DeliveryAccum {
    std::array<uint8_t, 16> delivery_id{};
    uint64_t                bytes   = 0;
    uint64_t                last_ms = 0;
};
constexpr uint64_t kDeliveryIdleMs = 5000;   // flush a delivery this idle
std::mutex                                       g_delivery_mu;
std::unordered_map<std::string, DeliveryAccum>   g_delivery_accum;

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
// Most recent load_score (0..1) each mini-node peer reported in their
// `mini.hello` heartbeat. mininodes.list embeds this so a bootstrapping
// browser/player can pick the least-busy VPS to land on. A peer that
// never reported stays at 0.0 (== effectively "idle / unknown"), which
// matches how routes treat the same field.
std::unordered_map<std::string, float>        g_mininode_load;

// ---- Sparse-mesh scaling knobs --------------------------------------
//
// The default mini-node topology is a FULL MESH: every mini heartbeats
// every other mini, replicates every route to all of them, and returns
// the entire peer set in mininodes.list. That's O(N^2) control-plane
// traffic and is fine at today's handful of VPSes but does not survive
// into the thousands of minis.
//
// When `g_sparse_mesh` is enabled the mesh becomes a bounded-degree
// epidemic gossip overlay instead:
//   * route replication + the mini.hello heartbeat fan out to at most
//     `g_mesh_fanout` randomly-sampled peers per round (not all of them);
//   * routes are re-forwarded from ANY source (multi-hop) with a
//     content-signature dedup so the flood still reaches every mini but
//     terminates instead of looping. Coverage is high-probability
//     complete once fanout >= ~ln(N), so ~16 covers tens of thousands.
// Disabled by default: at N <= fanout it is a strict no-op, so the live
// deployment behaves exactly as before until an operator opts in.
bool g_sparse_mesh = false;
int  g_mesh_fanout = 16;   // per-round fan-out when sparse mesh is on
// mininodes.list is always capped to a random sample of this many peers
// (+ self). A player only needs a handful of failover targets, never the
// whole set. Safe to leave active: no live deployment has this many minis,
// so at small N it returns everyone exactly like before.
int  g_mininodes_list_max = 64;

// Content-signature dedup for epidemic route forwarding. Keyed by a hash
// of the exact route body we'd re-broadcast, so an identical route seen
// again within the window is dropped (this is what terminates the flood).
std::mutex                            g_route_gossip_mu;
std::unordered_map<size_t, uint64_t>  g_route_gossip_seen; // body-hash -> last-fwd ms
constexpr uint64_t kRouteGossipDedupMs = 5 * 60 * 1000;    // re-flood window
constexpr size_t   kRouteGossipSeenCap = 8192;             // prune above this

// ---- Route authentication -------------------------------------------
//
// Full nodes sign every published route (envelope {route,pubkey,sig}); the
// mini verifies address_from_pubkey(pubkey) == node_id + the ECDSA sig
// before trusting or relaying it. A bad signature is ALWAYS dropped. When
// `g_require_signed_routes` is on, unsigned (legacy) routes are dropped too;
// default off so a mixed fleet keeps working during rollout. Verification
// needs only the public key carried in the route — no private key on the VPS.
bool g_require_signed_routes = false;
// Replay window for signed routes (their `ts` is unix seconds). Republish is
// every ~15 min, so 30 min tolerates one missed cycle; 5 min covers skew.
constexpr uint64_t kRouteMaxAgeSec       = 30 * 60;
constexpr uint64_t kRouteFutureSkewSec   = 5 * 60;

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
// (#2 instability fix) Hard caps on the chat maps. on_chat_room_announce is
// unauthenticated and used to auto-subscribe + allocate a deque per distinct
// room name with no limit — a remote peer could announce unbounded room
// names to exhaust VPS memory + gossipsub subscriptions. librats (frozen)
// exposes no unsubscribe, so the only durable bound is to refuse rooms beyond
// a cap (rather than evict-and-leak-the-subscription). Body size is capped so
// one message can't pin arbitrary memory in the ring.
constexpr size_t      kMaxChatRooms     = 128;
constexpr size_t      kMaxChatBodyBytes = 8 * 1024;

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

// ---- Chat moderation + push state (all memory-only, bounded) --------
//
// Wire protocol (must match the player byte-for-byte). Every chat action
// is wallet-signed; the canonical preimage uses a 6-byte ASCII domain
// tag, raw fields, and 0x1F (unit separator) between variable-length
// fields, u64 little-endian for timestamps. NO pre-hashing —
// crypto::verify_data() sha256's internally then verify_ecdsa.
constexpr const char* kChatModTopic = "chat:mod";
constexpr uint8_t     kUnitSep      = 0x1F;

// Per-room ban list (set of 0x-prefixed lowercase wallet hex addresses
// the room creator / a global moderator has kicked). Bounded by
// kMaxChatRooms rooms; per-room set is small in practice.
std::mutex                                                      g_chat_bans_mu;
std::unordered_map<std::string, std::unordered_set<std::string>> g_chat_bans;

// Dedup set of moderation-action signatures already processed + re-published
// to chat:mod, so a re-broadcast doesn't loop. TTL-evicted by the reaper.
constexpr size_t   kMaxChatModSeen   = 4096;
constexpr uint64_t kChatModSeenTtlMs = 10 * 60 * 1000; // 10 min
std::mutex                                  g_chat_mod_seen_mu;
std::unordered_map<std::string, uint64_t>   g_chat_mod_seen; // sig hex -> first_seen_ms

// Per-player watched rooms: peer_id -> set<room>. A player calls
// chat.watch{room} so the mini-node pushes live messages for that room to
// it (chat.message push), and chat.unwatch{room} to stop. Bounded by the
// connected-peer count; cleaned on disconnect.
constexpr size_t kMaxWatchedRoomsPerPlayer = 64;
std::mutex                                                      g_chat_watchers_mu;
std::unordered_map<std::string, std::unordered_set<std::string>> g_chat_watchers;

// Cached active global-moderator set (lowercase 20-byte hex, no 0x), pulled
// periodically from a full node via the "mod.list_moderators" RPC. chat.moderate
// authorizes a non-creator actor only if their address is in here.
constexpr uint64_t kChatModsTtlMs = 5 * 60 * 1000; // 5 min refresh window
std::mutex                          g_chat_mods_mu;
std::unordered_set<std::string>     g_chat_mods;            // lowercase hex, no 0x
uint64_t                            g_chat_mods_fetched_ms = 0;

// Forward declarations; bodies appear below the rats_client_t globals.
void chat_subscribe_room(rats_client_t client, const std::string& name);
void on_chat_room_announce(void*, const char*, const char*, const char*);
void on_chat_message(void*, const char*, const char*, const char*);
void on_chat_mod_message(void*, const char*, const char*, const char*);

// ---- Chat signature verification (shared signed wire protocol) ------
//
// Common helper: parse a 66-hex compressed pubkey + a 128-hex sig, require
// crypto::address_from_pubkey(pubkey) == the claimed 20-byte address, then
// crypto::verify_data(canon, canon_len, sig, pubkey). Returns true on a
// fully valid envelope. `claimed_addr_hex` is the 0x-prefixed/lowercase
// 20-byte hex from the message's address field (from/creator/by).
bool chat_verify_canon(const std::vector<uint8_t>& canon,
                       const std::string& claimed_addr_hex,
                       const std::string& pubkey_hex,
                       const std::string& sig_hex) {
    if (pubkey_hex.size() != 66 || sig_hex.size() != 128) return false;
    auto pk_bytes  = mc::crypto::from_hex(pubkey_hex);
    auto sig_bytes = mc::crypto::from_hex(sig_hex);
    if (pk_bytes.size() != 33 || sig_bytes.size() != 64) return false;
    mc::Address claimed{};
    if (!mc::crypto::parse_address_checksummed(claimed_addr_hex, claimed))
        return false;
    mc::PubKey33 pub{};
    std::copy(pk_bytes.begin(), pk_bytes.end(), pub.begin());
    mc::Sig64 sig{};
    std::copy(sig_bytes.begin(), sig_bytes.end(), sig.begin());
    // Require the pubkey to actually derive the claimed wallet address.
    if (mc::crypto::address_from_pubkey(pub) != claimed) return false;
    return mc::crypto::verify_data(canon.data(), canon.size(), sig, pub);
}

// Verify a signed route envelope. `inner_route` is the exact route JSON the
// publisher signed; `node_id_hex` is its node_id (== the publisher's 40-hex
// wallet address). Returns true iff the pubkey/sig are well-formed, the
// pubkey derives node_id, and the ECDSA sig covers the domain-separated
// inner bytes. Same trust model as chat: identity is bound to the wallet
// key, so a peer cannot forge a route for a node_id it doesn't control, nor
// tamper with any signed field in transit. Domain prefix must match the
// signer in rats_link.cpp::build_route_message.
bool route_verify(const std::string& inner_route,
                  const std::string& node_id_hex,
                  const std::string& pubkey_hex,
                  const std::string& sig_hex) {
    if (pubkey_hex.size() != 66 || sig_hex.size() != 128) return false;
    auto pk_bytes  = mc::crypto::from_hex(pubkey_hex);
    auto sig_bytes = mc::crypto::from_hex(sig_hex);
    if (pk_bytes.size() != 33 || sig_bytes.size() != 64) return false;
    mc::Address claimed{};
    if (!mc::crypto::parse_address_checksummed(node_id_hex, claimed)) return false;
    mc::PubKey33 pub{};
    std::copy(pk_bytes.begin(), pk_bytes.end(), pub.begin());
    mc::Sig64 sig{};
    std::copy(sig_bytes.begin(), sig_bytes.end(), sig.begin());
    if (mc::crypto::address_from_pubkey(pub) != claimed) return false;
    static const std::string kRouteSigDomain = "bopwire-route-v1:";
    const std::string preimage = kRouteSigDomain + inner_route;
    return mc::crypto::verify_data(
        reinterpret_cast<const uint8_t*>(preimage.data()), preimage.size(),
        sig, pub);
}

// Append a u64 little-endian to a byte vector.
inline void append_u64_le(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(uint8_t(x >> (8 * i)));
}

// Append a 20-byte raw address parsed from 0x-prefixed/lowercase hex.
// Returns false if the address doesn't parse.
inline bool append_addr_raw(std::vector<uint8_t>& v, const std::string& hex) {
    mc::Address a{};
    if (!mc::crypto::parse_address_checksummed(hex, a)) return false;
    v.insert(v.end(), a.begin(), a.end());
    return true;
}

// Build the "mccht1" CHAT MESSAGE preimage:
//   "mccht1" || room(utf8) || 0x1F || from(20 raw) || ts_ms(u64 LE) || body(utf8)
bool build_chat_msg_canon(const std::string& room, const std::string& from_hex,
                          uint64_t ts_ms, const std::string& body,
                          std::vector<uint8_t>& out) {
    out.clear();
    static const char tag[] = "mccht1";
    out.insert(out.end(), tag, tag + 6);
    out.insert(out.end(), room.begin(), room.end());
    out.push_back(kUnitSep);
    if (!append_addr_raw(out, from_hex)) return false;
    append_u64_le(out, ts_ms);
    out.insert(out.end(), body.begin(), body.end());
    return true;
}

// Build the "mccrm1" ROOM CREATE preimage:
//   "mccrm1" || name(utf8) || 0x1F || creator(20 raw) || created_ms(u64 LE) || private_byte
bool build_chat_room_canon(const std::string& name, const std::string& creator_hex,
                           uint64_t created_ms, bool is_private,
                           std::vector<uint8_t>& out) {
    out.clear();
    static const char tag[] = "mccrm1";
    out.insert(out.end(), tag, tag + 6);
    out.insert(out.end(), name.begin(), name.end());
    out.push_back(kUnitSep);
    if (!append_addr_raw(out, creator_hex)) return false;
    append_u64_le(out, created_ms);
    out.push_back(is_private ? 0x01 : 0x00);
    return true;
}

// Build the "mccmd1" MOD ACTION preimage:
//   "mccmd1" || action(utf8) || 0x1F || room(utf8) || 0x1F || target(utf8) || 0x1F || by(20 raw) || ts_ms(u64 LE)
bool build_chat_mod_canon(const std::string& action, const std::string& room,
                          const std::string& target, const std::string& by_hex,
                          uint64_t ts_ms, std::vector<uint8_t>& out) {
    out.clear();
    static const char tag[] = "mccmd1";
    out.insert(out.end(), tag, tag + 6);
    out.insert(out.end(), action.begin(), action.end());
    out.push_back(kUnitSep);
    out.insert(out.end(), room.begin(), room.end());
    out.push_back(kUnitSep);
    out.insert(out.end(), target.begin(), target.end());
    out.push_back(kUnitSep);
    if (!append_addr_raw(out, by_hex)) return false;
    append_u64_le(out, ts_ms);
    return true;
}

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

// Wall-clock milliseconds. ONLY for on-the-wire timestamps (route `ts`,
// chat ts_ms, event-log display) where the value crosses process / host
// boundaries and must be comparable against another machine's clock. NOT
// for interval / TTL / refill math — a wall-clock step (NTP slew, manual
// set, leap) would either expire pending relays early or stall the reaper.
uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// (#8) Monotonic milliseconds since an arbitrary epoch. Used for every
// duration comparison in this file (route TTL, pending-relay TTL, token-
// bucket refill, delivery-idle flush, mini.hello heartbeat). steady_clock
// never goes backwards, so these never mis-fire on a wall-clock jump.
uint64_t mono_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
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
    out << "\033[1m bopwire mini-node \033[0m"
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
// #10: retain the full keypair so the mini-node can SIGN relay.report
// (the same key that derives g_wallet_address_hex, so the broker verifies
// recover(sig) == the wallet it already knows for this peer).
std::vector<uint8_t> g_mini_priv;          // 32-byte secp256k1 private
mc::PubKey33         g_mini_pub{};         // 33-byte compressed public
mc::Address          g_mini_addr20{};      // raw 20-byte address (sign preimage)

std::string routes_json() {
    std::ostringstream ss;
    mc::net::LoadMonitor::Snapshot self_load{};
    if (g_load_mon) self_load = g_load_mon->current();
    // Top-level self_load_score is the scalar version the browser /
    // player bootstrapping code reads to pick the least-busy mini-node
    // (it doesn't need the full breakdown). The full self_load object
    // stays for debug RPC + operator dashboards.
    ss << "{\"self_load_score\":" << self_load.load_score        << ","
       << "\"self_load\":{"
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
           // received_at_ms is monotonic (#8) — not comparable to a player's
           // wall clock. Convert the monotonic age back to a wall-clock
           // last-seen so the on-the-wire field stays meaningful cross-host.
           << "\"last_seen_ms\":"
           << (now_ms() - (mono_ms() - r.received_at_ms))
           << "}";
    }
    ss << "]}";
    return ss.str();
}

// ---- Reachability probe ---------------------------------------------
//
// For each route we receive, spawn a one-shot worker that brings up a fresh
// librats client on an OS-picked ephemeral port and tries to connect to the
// node's STUN-discovered public_address. If the handshake completes within
// `kProbeTimeoutMs`, the node's NAT lets independent flows in (full-cone or
// restricted-cone-with-permissive-mapping) → reachability = direct.
// If the handshake times out, peer-to-peer traffic must be relayed through
// this mini-node instead → reachability = relay.

constexpr int kProbeTimeoutMs = 3000;

void mark_reachability(const std::string& node_id, Reachability r) {
    std::lock_guard<std::mutex> lk(g_routes_mu);
    auto it = g_routes.find(node_id);
    if (it == g_routes.end()) return;
    it->second.reachability             = r;
    it->second.reachability_tested_at_ms = mono_ms(); // (#8) interval clock
    if (!g_quiet) {
        push_event("reach", it->second.node_id, reachability_str(r));
    }
}

// (#10 instability fix) bound concurrent reachability probes. Each probe
// spawns a full librats client + a 3s handshake wait; an unthrottled
// route-gossip burst would otherwise spawn unbounded detached threads/clients
// and exhaust the VPS (fd/thread/memory). Over the cap we skip — reachability
// stays Unknown and ingest_route's staleness check re-probes it later.
std::atomic<int> g_active_probes{0};
constexpr int    kMaxConcurrentProbes = 8;
// wallet-as-id: monotonic counter for per-probe throwaway librats ids (see
// probe_reachability) so a reachability probe can never share — and thus evict —
// the mini's wallet-derived hub identity.
std::atomic<uint64_t> g_probe_seq{0};

void probe_reachability(std::string node_id, std::string public_address) {
    if (g_active_probes.fetch_add(1) >= kMaxConcurrentProbes) {
        g_active_probes.fetch_sub(1);
        return;
    }
    std::thread([node_id = std::move(node_id),
                 public_address = std::move(public_address)]() mutable {
        // Release the probe slot on every exit path.
        struct SlotGuard { ~SlotGuard() { g_active_probes.fetch_sub(1); } } slot_guard;
        if (public_address.empty()) return;
        const auto colon = public_address.find(':');
        if (colon == std::string::npos) return;
        const std::string host = public_address.substr(0, colon);
        int port;
        try { port = std::stoi(public_address.substr(colon + 1)); }
        catch (...) { return; }

        // Distinct throwaway identity per probe so it can NEVER share the mini's
        // wallet-derived hub id — otherwise librats would evict the hub's real
        // connection to the node in favour of this short-lived probe socket
        // (self-inflicted decapitation). 24 hex of our wallet namespaces it
        // per-mini; a 16-hex monotonic counter makes it unique per probe.
        std::ostringstream probe_tail;
        probe_tail << std::hex << std::setw(16) << std::setfill('0')
                   << g_probe_seq.fetch_add(1);
        const std::string probe_id =
            (g_wallet_address_raw.size() == 40 ? g_wallet_address_raw.substr(0, 24)
                                               : std::string(24, '0'))
            + probe_tail.str();
        rats_client_t probe = rats_create_with_id(0, probe_id.c_str()); // ephemeral port, distinct id
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

// Random sample of up to `k` elements from `all` (Fisher-Yates prefix). Used
// to bound mesh fan-out + mininodes.list. Returns `all` unchanged when it
// already fits, so at small N (N <= k) this is a no-op.
std::vector<std::string> sample_up_to(std::vector<std::string> all, size_t k) {
    if (k == 0 || all.size() <= k) return all;
    static thread_local std::mt19937 rng{std::random_device{}()};
    for (size_t i = 0; i < k; ++i) {
        std::uniform_int_distribution<size_t> pick(i, all.size() - 1);
        std::swap(all[i], all[pick(rng)]);
    }
    all.resize(k);
    return all;
}

// Epidemic-gossip dedup: true the first time we see this exact route body
// (within kRouteGossipDedupMs), false for a repeat. Re-forwarding only the
// first sighting is what makes multi-hop flooding terminate instead of
// ping-ponging forever around a bounded mesh.
bool route_gossip_should_forward(const std::string& body) {
    const size_t   h   = std::hash<std::string>{}(body);
    const uint64_t now = now_ms();
    std::lock_guard<std::mutex> lk(g_route_gossip_mu);
    auto it = g_route_gossip_seen.find(h);
    if (it != g_route_gossip_seen.end() && now - it->second < kRouteGossipDedupMs)
        return false;
    // Opportunistic prune so the seen-map can't grow without bound.
    if (g_route_gossip_seen.size() > kRouteGossipSeenCap) {
        for (auto i = g_route_gossip_seen.begin(); i != g_route_gossip_seen.end();) {
            if (now - i->second >= kRouteGossipDedupMs) i = g_route_gossip_seen.erase(i);
            else ++i;
        }
    }
    g_route_gossip_seen[h] = now;
    return true;
}

// Mini-node mesh helpers — defined further down once g_client is in scope.
// Declared here because ingest_route uses them to fan a freshly-received
// route out to every other mini-node we know about.
// (#6) ingest_route enqueues its fan-outs onto the off-io sender queue, whose
// definition lives just below g_client — forward-declare it here.
void enqueue_send_task(std::function<void()> task);
void send_mini_hello(const std::string& peer_id);
void replicate_routes_to_peer(const std::string& peer_id);
void replicate_route_to_mininodes(const std::string& route_json,
                                  const std::string& source_peer_id);
bool peer_is_mininode(const std::string& peer_id);
void broadcast_swarm_peer_offline(const std::string& disconnected_pid);
void broadcast_swarm_peer_online(const std::string& online_pid);
void replay_online_players_to_home(const std::string& full_pid);

// Chat push + moderation helpers — defined once g_client is in scope.
void chat_push_message_to_watchers(const std::string& room,
                                   const nlohmann::json& msg_envelope);
void chat_refresh_global_mods_if_stale();

// Re-test reachability if the last probe is older than this.
constexpr uint64_t kProbeMinIntervalMs = 60'000;

void ingest_route(const std::string& body, const char* peer_id) {
    // --- Route authentication (see route_verify / g_require_signed_routes).
    // A signed route arrives as an envelope {route,pubkey,sig} where `route`
    // is the exact inner JSON the publisher signed. Verify before trusting.
    // Legacy full nodes still send the bare inner JSON — accepted only while
    // enforcement is off.
    std::string inner = body;   // the route JSON we parse below
    std::string signed_env;     // verified envelope, kept for re-replication
    {
        bool is_env = false;
        nlohmann::json outer;
        try {
            outer = nlohmann::json::parse(body);
            is_env = outer.is_object() && outer.contains("route") &&
                     outer["route"].is_string() && outer.contains("pubkey") &&
                     outer.contains("sig");
        } catch (...) { /* not JSON -> handled as legacy below */ }

        if (is_env) {
            const std::string route_str = outer["route"].get<std::string>();
            const std::string pubkey    = outer.value("pubkey", std::string());
            const std::string sig       = outer.value("sig",    std::string());
            const std::string claimed   = json_get_string(route_str, "node_id");
            if (!route_verify(route_str, claimed, pubkey, sig)) {
                if (!g_quiet)
                    std::cout << "[routes] DROP bad-signature route node="
                              << claimed.substr(0, 12) << "\n";
                return;   // forged/tampered — never accept, even when lenient
            }
            inner      = route_str;   // verified bytes
            signed_env = body;        // verbatim envelope for re-forwarding
        } else if (g_require_signed_routes) {
            if (!g_quiet)
                std::cout << "[routes] DROP unsigned route (enforcing)\n";
            return;
        }
    }

    RouteEntry e;
    e.node_id         = json_get_string(inner, "node_id");
    e.rats_peer_id    = json_get_string(inner, "rats_peer_id");
    e.public_address  = json_get_string(inner, "public_address");
    e.api_port        = static_cast<int>(json_get_uint(inner, "api_port"));
    e.ts              = json_get_uint(inner, "ts");
    e.received_at_ms  = mono_ms(); // (#8) drives the route TTL — monotonic
    e.signed_envelope = signed_env;
    // Load fields — parse via nlohmann::json so floats and bools work, the
    // hand-rolled json_get_string used above is string-only.
    try {
        auto j = nlohmann::json::parse(inner);
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

    // Replay guard for signed routes: reject stale / far-future timestamps so
    // a captured old envelope can't be re-injected. (`ts` is unix seconds.)
    if (!signed_env.empty()) {
        const uint64_t now_s = now_ms() / 1000;
        if (e.ts + kRouteMaxAgeSec < now_s || e.ts > now_s + kRouteFutureSkewSec) {
            if (!g_quiet)
                std::cout << "[routes] DROP stale/future signed route node="
                          << e.node_id.substr(0, 12) << "\n";
            return;
        }
    }

    bool need_probe = false;
    {
        std::lock_guard<std::mutex> lk(g_routes_mu);
        auto it = g_routes.find(e.node_id);
        if (it == g_routes.end()) {
            need_probe = true;
        } else {
            // Replay guard: never let an older signed route overwrite a newer
            // signed one we already trust (lock released by RAII on return).
            if (!it->second.signed_envelope.empty() && e.ts < it->second.ts)
                return;
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

    // Replicate to other mini-nodes. (#6) The fan-out runs on the sender
    // thread so ingest (on the io thread) returns promptly.
    //   * Full-mesh (default): forward UNLESS the sender is itself a
    //     mini-node — a mini source means we got this as a one-hop replica,
    //     so re-forwarding would loop.
    //   * Sparse mesh: multi-hop epidemic gossip — forward from ANY source
    //     but only the first time we've seen this exact body, which reaches
    //     minis we aren't directly meshed with yet terminates the flood.
    const std::string source = peer_id ? peer_id : "";
    const bool do_forward = g_sparse_mesh ? route_gossip_should_forward(body)
                                          : !peer_is_mininode(source);
    if (do_forward) {
        std::string body_copy = body, src_copy = source;
        enqueue_send_task([body_copy, src_copy]() {
            replicate_route_to_mininodes(body_copy, src_copy);
        });
    }

    // Full node just (re)published its route to us, which means it
    // either reconnected or is brand new on the mesh. Replay every
    // currently-online player so its swarm view recovers without
    // waiting up to 5 minutes for each player's next swarm.hello tick.
    // Only fires for direct sources — relayed routes (from another
    // mini-node) don't have the full node directly available to us.
    // (#6) Replay loops over every g_players entry → off-io.
    if (!source.empty() && !peer_is_mininode(source)) {
        if (!e.rats_peer_id.empty()) {
            std::string full_pid = e.rats_peer_id;
            enqueue_send_task([full_pid]() {
                replay_online_players_to_home(full_pid);
            });
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
        g_mininode_load.erase(peer_id);
    }
    // Drop the player record so the next route-publish from a home
    // node doesn't replay an offline peer back as "online". (#7) Capture
    // whether this peer was actually a registered PLAYER — the erase return
    // is the authoritative signal, since only player.announce inserts here.
    bool was_player = false;
    if (peer_id && !was_mininode) {
        std::lock_guard<std::mutex> lk(g_players_mu);
        was_player = g_players.erase(peer_id) > 0;
    }
    // Drop any chat watch subscriptions this peer held so the watcher map
    // stays bounded to currently-connected players.
    if (peer_id) {
        std::lock_guard<std::mutex> lk(g_chat_watchers_mu);
        g_chat_watchers.erase(peer_id);
    }
    // (#7 symmetric gating) Only broadcast peer_offline for a peer we
    // previously broadcast ONLINE — i.e. a registered player. Firing for
    // every non-mini-node disconnect (full nodes, half-open connections,
    // never-announced peers) sent the full nodes spurious "offline" events
    // for peer_ids they never saw as "online", and let a transient full-node
    // blip evict relayed players' visibility. The mesh handles mini-node
    // churn via reconnect + mini.hello, so they're excluded too.
    if (peer_id && *peer_id && was_player) {
        // (#6) off-io: the broadcast loops over every validated peer; running
        // it inline would stall the io thread mid-disconnect-callback.
        std::string pid(peer_id);
        enqueue_send_task([pid]() { broadcast_swarm_peer_offline(pid); });
    }
    // (#1 instability fix) Purge any pending relay forwards owned by the
    // departed originator — its reply would be black-holed anyway (the
    // transport peer_id is gone), so dropping them now bounds g_pending_relays
    // immediately on the disconnect instead of waiting out the TTL reaper.
    if (peer_id && *peer_id) {
        std::lock_guard<std::mutex> lk(g_relay_mu);
        for (auto it = g_pending_relays.begin(); it != g_pending_relays.end(); ) {
            if (it->second.originator_peer_id == peer_id) {
                it = g_pending_relays.erase(it);
            } else { ++it; }
        }
    }
}

// ---- librats RPC handler --------------------------------------------
//
// Players ask us for the current routing table with:
//   {req_id, type:"routes.get", body:{}}
// We answer on `bopwire.reply` with the same JSON list we used to serve
// over the HTTP /dht-peers endpoint.

rats_client_t g_client = nullptr;

void send_reply(const char* peer_id, const std::string& reply_json) {
    if (!g_client || !peer_id) return;
    rats_send_message(g_client, peer_id, kReplyType, reply_json.c_str());
}

// ---- Off-io sender work queue (#6) ----------------------------------
//
// CRITICAL: librats runs accept + recv + decrypt + every on_* callback on a
// SINGLE io thread. The swarm broadcasts and replicate/replay fan-outs below
// each loop over every validated peer doing a synchronous rats_send_message;
// run inline from on_peer_disconnected / on_relay_reply / ingest_route they
// stall that io thread (and thus ALL peers' recv) for the whole fan-out.
//
// So we hand each fan-out to this single-consumer queue, drained by ONE
// dedicated sender thread. The task is a std::function that performs the
// actual rats_send_* loop off the io thread.
//
// LOCK ORDER: g_send_q_mu is a LEAF mutex — held only to push/pop the deque,
// never while any rats_send_* call or any other g_* lock is held. The sender
// thread takes a task, RELEASES g_send_q_mu, THEN runs the task (which may
// take g_routes_mu / g_mininodes_mu / g_players_mu and call rats_send_*).
// This keeps it strictly below tx_mutex -> peers_mutex_ -> io_mutex_ and the
// data-structure mutexes; it never sits above any of them.
std::mutex                         g_send_q_mu;
std::condition_variable            g_send_q_cv;
std::deque<std::function<void()>>  g_send_q;
std::atomic<bool>                  g_sender_running{false};
std::thread                        g_sender_thread;
// Bound the queue so a pathological burst of disconnects can't grow it without
// limit. Past the cap we drop the OLDEST task (the fan-outs are best-effort
// catch-up; the swarm re-converges on the next periodic tick anyway).
constexpr size_t kMaxSendQueue = 4096;

void enqueue_send_task(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lk(g_send_q_mu);
        if (g_send_q.size() >= kMaxSendQueue) {
            g_send_q.pop_front();
        }
        g_send_q.push_back(std::move(task));
    }
    g_send_q_cv.notify_one();
}

void sender_loop() {
    std::unique_lock<std::mutex> lk(g_send_q_mu);
    while (g_sender_running.load()) {
        g_send_q_cv.wait(lk, [] {
            return !g_sender_running.load() || !g_send_q.empty();
        });
        while (!g_send_q.empty()) {
            std::function<void()> task = std::move(g_send_q.front());
            g_send_q.pop_front();
            // Release the leaf lock before running the task — the task takes
            // other g_* locks and calls rats_send_*; holding g_send_q_mu across
            // that would invert the leaf ordering and serialize producers.
            lk.unlock();
            try { task(); } catch (...) { /* never let one task kill the loop */ }
            lk.lock();
        }
    }
}

// ---- Mini-node mesh helpers -----------------------------------------

void send_mini_hello(const std::string& peer_id) {
    if (!g_client || peer_id.empty()) return;
    // Body carries the mini-node's wallet address so the full node can
    // attribute relay credits (RelayRewardTx target) to the right
    // wallet via RelayCreditTracker. Empty wallet falls through to a
    // bodyless hello — older full nodes ignore the field; newer ones
    // skip credit accounting for this peer until a wallet shows up.
    //
    // Also carries our current load_score so the receiving mini-node
    // can publish it in its mininodes.list reply — players use that to
    // bootstrap onto the least-busy VPS. Re-sent periodically by the
    // heartbeat loop in main() to keep the score reasonably fresh.
    nlohmann::json body = nlohmann::json::object();
    if (!g_wallet_address_hex.empty()) {
        body["wallet"] = g_wallet_address_hex;
    }
    mc::net::LoadMonitor::Snapshot snap{};
    if (g_load_mon) snap = g_load_mon->current();
    body["load_score"] = snap.load_score;
    body["cpu_load"]   = snap.cpu_load;
    body["net_bps"]    = snap.net_bytes_per_sec;
    body["is_busy"]    = snap.is_busy;
    nlohmann::json req = {
        {"req_id", new_relay_req_id()},
        {"type",   kMiniHelloType},
        {"body",   body},
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
        // Forward the verbatim signed envelope when we have one so the peer
        // can independently verify; fall back to the flat legacy record for
        // unsigned routes (which the peer only keeps if it isn't enforcing).
        if (!e.signed_envelope.empty()) {
            rats_send_message(g_client, peer_id.c_str(), kRoutesTopic,
                              e.signed_envelope.c_str());
            continue;
        }
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
    // Sparse mesh: fan out to at most g_mesh_fanout random peers per hop.
    // Epidemic multi-hop + dedup still covers the network; O(fanout) per node
    // instead of O(N). No-op at N <= fanout, so full mesh is unaffected.
    if (g_sparse_mesh)
        peers = sample_up_to(std::move(peers), static_cast<size_t>(g_mesh_fanout));
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
        const std::string creator_pubkey = j.value("creator_pubkey", std::string());
        const std::string sig            = j.value("sig",            std::string());
        // (#1) Verify the "mccrm1" envelope so only validly-signed rooms
        // allocate a deque + gossipsub subscription — closes the
        // unauthenticated-announce memory note. Drop on any failure.
        std::vector<uint8_t> canon;
        if (!build_chat_room_canon(room.name, room.creator, room.created_ms,
                                   room.is_private, canon))
            return;
        if (!chat_verify_canon(canon, room.creator, creator_pubkey, sig))
            return;
        bool added = false;
        {
            std::lock_guard<std::mutex> lk(g_chat_rooms_mu);
            // (#2) refuse new rooms past the cap so subscriptions + deques
            // stay bounded; existing rooms still update.
            const bool known = g_chat_rooms.find(room.name) != g_chat_rooms.end();
            if (known || g_chat_rooms.size() < kMaxChatRooms) {
                added = g_chat_rooms.emplace(room.name, room).second;
            } else if (!g_quiet) {
                std::cerr << "[mini-node] chat-room cap (" << kMaxChatRooms
                          << ") reached — ignoring '" << room.name << "'\n";
            }
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
        if (m.body.size() > kMaxChatBodyBytes) return;   // (#2) drop oversized
        // (#1) Verify the "mccht1" preimage; drop on failure so only
        // wallet-signed messages ever enter the ring.
        std::vector<uint8_t> canon;
        if (!build_chat_msg_canon(room_name, m.from, m.ts_ms, m.body, canon))
            return;
        if (!chat_verify_canon(canon, m.from, m.from_pubkey, m.sig))
            return;
        // Drop messages from banned senders so a kicked wallet can't keep
        // talking via the gossip mesh.
        {
            std::lock_guard<std::mutex> lk(g_chat_bans_mu);
            auto bit = g_chat_bans.find(room_name);
            if (bit != g_chat_bans.end() && bit->second.count(m.from) > 0) return;
        }
        {
            std::lock_guard<std::mutex> lk(g_chat_msgs_mu);
            // (#2) don't allocate a new room deque past the cap.
            if (g_chat_msgs.find(room_name) == g_chat_msgs.end() &&
                g_chat_msgs.size() >= kMaxChatRooms) return;
            auto& dq = g_chat_msgs[room_name];
            dq.push_back(m);
            while (dq.size() > kChatRingPerRoom) dq.pop_front();
        }
        // Push to any players currently watching this room so they see the
        // message live (the player reads these via its onPush hook).
        chat_push_message_to_watchers(room_name, j);
    } catch (const nlohmann::json::exception&) {
        // Malformed message — drop.
    }
}

// Push a chat message to every player currently watching `room`. The push
// reuses the same fire-and-forget mechanism the relay.push.forward verb
// uses: a `bopwire.reply` (kReplyType) envelope carrying a FRESH req_id
// the player never issued, so the player's _dispatchReply finds no pending
// match and routes it to its onPush hook as type "chat.message". The body
// is the verbatim signed message envelope ({room,from,from_pubkey,ts_ms,
// body,sig}) so the player can re-verify the signature locally.
void chat_push_message_to_watchers(const std::string& room,
                                   const nlohmann::json& msg_envelope) {
    if (!g_client) return;
    std::vector<std::string> watchers;
    {
        std::lock_guard<std::mutex> lk(g_chat_watchers_mu);
        for (const auto& [pid, rooms] : g_chat_watchers) {
            if (rooms.count(room) > 0) watchers.push_back(pid);
        }
    }
    if (watchers.empty()) return;
    // Ensure the room name travels with the push so the player can route
    // the message to the right channel even if the envelope omitted it.
    nlohmann::json body = msg_envelope;
    if (!body.contains("room")) body["room"] = room;
    nlohmann::json push = {
        {"req_id", new_relay_req_id()},
        {"type",   "chat.message"},
        {"body",   body},
    };
    const std::string payload = push.dump();
    for (const auto& pid : watchers) {
        rats_send_message(g_client, pid.c_str(), kReplyType, payload.c_str());
    }
}

// Refresh the cached global-moderator set from a full node, at most once
// per kChatModsTtlMs. We ask the first full node in g_routes via the
// "mod.list_moderators" RPC over our existing QUIC link (the same
// kRequestType path we use to relay). The reply lands on kReplyType and is
// absorbed by on_relay_reply, so we DON'T correlate it here — instead the
// reply handler stuffs the moderator set into g_chat_mods. We just fire the
// request when the cache is stale. Best-effort: if no full node is known we
// simply keep whatever we have (creator-only auth still works).
void chat_refresh_global_mods_if_stale() {
    if (!g_client) return;
    const uint64_t now = now_ms();
    {
        std::lock_guard<std::mutex> lk(g_chat_mods_mu);
        if (g_chat_mods_fetched_ms != 0 &&
            now - g_chat_mods_fetched_ms < kChatModsTtlMs) {
            return; // still fresh
        }
        // Optimistically stamp now so concurrent callers don't all fire.
        g_chat_mods_fetched_ms = now;
    }
    std::string target;
    {
        std::lock_guard<std::mutex> lk(g_routes_mu);
        for (const auto& kv : g_routes) {
            const std::string& pid = kv.second.rats_peer_id.empty()
                ? kv.second.node_id : kv.second.rats_peer_id;
            if (!pid.empty()) { target = pid; break; }
        }
    }
    if (target.empty()) return;
    nlohmann::json req = {
        {"req_id", "chatmods-" + new_relay_req_id()},
        {"type",   "mod.list_moderators"},
        {"body",   nlohmann::json::object()},
    };
    rats_send_message(g_client, target.c_str(), kRequestType,
                      req.dump().c_str());
}

// Lowercase + strip an optional 0x prefix from a 20-byte hex address so two
// addresses compare stably regardless of EIP-55 checksum casing.
std::string chat_norm_addr(std::string s) {
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s = s.substr(2);
    for (auto& ch : s)
        ch = static_cast<char>(std::tolower((unsigned char)ch));
    return s;
}

// Apply a verified, authorized "mccmd1" moderation action to local state.
// Shared by the chat.moderate verb and the chat:mod gossip callback. Does
// NOT verify or authorize — callers must do that first.
void chat_apply_mod_action(const std::string& action, const std::string& room,
                           const std::string& target, const std::string& by) {
    if (action == "kick_user") {
        if (target.empty()) return;
        {
            std::lock_guard<std::mutex> lk(g_chat_bans_mu);
            g_chat_bans[room].insert(target);
        }
        if (g_client) {
            std::vector<std::string> watchers;
            {
                std::lock_guard<std::mutex> lk(g_chat_watchers_mu);
                for (const auto& [pid, rooms] : g_chat_watchers)
                    if (rooms.count(room) > 0) watchers.push_back(pid);
            }
            nlohmann::json push = {
                {"req_id", new_relay_req_id()},
                {"type",   "chat.kicked"},
                {"body",   {{"room", room}, {"target", target}, {"by", by}}},
            };
            const std::string payload = push.dump();
            for (const auto& pid : watchers)
                rats_send_message(g_client, pid.c_str(), kReplyType,
                                  payload.c_str());
        }
    } else if (action == "remove_room") {
        { std::lock_guard<std::mutex> lk(g_chat_rooms_mu); g_chat_rooms.erase(room); }
        { std::lock_guard<std::mutex> lk(g_chat_msgs_mu);  g_chat_msgs.erase(room);  }
        { std::lock_guard<std::mutex> lk(g_chat_bans_mu);  g_chat_bans.erase(room);  }
        // librats (frozen) has no unsubscribe; the per-room subscription
        // leaks but the maps are freed — matches the existing cap note.
    }
}

// Gossip ingest for moderation actions published to the "chat:mod" topic by
// other mini-nodes. Verify "mccmd1", authorize (creator OR cached global
// mod), dedup on the signature, then apply locally. We do NOT re-publish
// here (the originating mini-node already did) — gossipsub fans it out.
void on_chat_mod_message(void* /*ud*/, const char* /*peer_id*/,
                         const char* /*topic*/, const char* msg) {
    if (!msg) return;
    try {
        auto j = nlohmann::json::parse(msg);
        const std::string action    = j.value("action",    std::string());
        const std::string room      = j.value("room",      std::string());
        const std::string target    = j.value("target",    std::string());
        const std::string by        = j.value("by",        std::string());
        const std::string by_pubkey = j.value("by_pubkey", std::string());
        const uint64_t    ts_ms     = j.value("ts_ms",     static_cast<uint64_t>(0));
        const std::string sig       = j.value("sig",       std::string());
        if ((action != "kick_user" && action != "remove_room") ||
            room.empty() || by.empty() || ts_ms == 0) return;
        // Dedup first (cheap) so a re-broadcast loop terminates.
        {
            std::lock_guard<std::mutex> lk(g_chat_mod_seen_mu);
            if (g_chat_mod_seen.find(sig) != g_chat_mod_seen.end()) return;
        }
        std::vector<uint8_t> canon;
        if (!build_chat_mod_canon(action, room, target, by, ts_ms, canon)) return;
        if (!chat_verify_canon(canon, by, by_pubkey, sig)) return;
        // Authorize: room creator OR cached global moderator.
        bool is_creator = false;
        {
            std::lock_guard<std::mutex> lk(g_chat_rooms_mu);
            auto it = g_chat_rooms.find(room);
            if (it != g_chat_rooms.end())
                is_creator = (chat_norm_addr(it->second.creator) ==
                              chat_norm_addr(by));
        }
        bool is_global_mod = false;
        {
            std::lock_guard<std::mutex> lk(g_chat_mods_mu);
            is_global_mod = g_chat_mods.count(chat_norm_addr(by)) > 0;
        }
        if (!is_creator && !is_global_mod) return;
        // Record the signature now that it's verified + authorized.
        {
            std::lock_guard<std::mutex> lk(g_chat_mod_seen_mu);
            if (g_chat_mod_seen.size() < kMaxChatModSeen)
                g_chat_mod_seen[sig] = now_ms();
        }
        chat_apply_mod_action(action, room, target, by);
    } catch (const nlohmann::json::exception&) {
        // Malformed — drop.
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
        // Pull the peer's reported load_score (added 2026-06: mini-nodes
        // periodically resend mini.hello so this score stays fresh).
        // Older peers don't include the field; default to 0.0 in that
        // case, same convention used for full-node routes that haven't
        // republished since they got the load extension.
        float peer_load = 0.0f;
        {
            const auto& inner = env.value("body", nlohmann::json::object());
            if (inner.contains("load_score") && inner["load_score"].is_number())
                peer_load = inner["load_score"].get<float>();
        }
        bool added = false;
        {
            std::lock_guard<std::mutex> lk(g_mininodes_mu);
            added = g_mininode_peers.insert(pid).second;
            if (!addr.empty()) g_mininode_addr[pid] = addr;
            g_mininode_load[pid] = peer_load;
        }
        if (added && !g_quiet) {
            push_event("vps-peer", pid, addr);
        }
        if (added) {
            // First time we've heard from this mini-node — kick off a
            // route snapshot so their cache catches up to ours. (#6) route it
            // through the sender thread instead of spawning a detached thread
            // per join (the snapshot loops over every route → off-io).
            const std::string pid_copy = pid;
            enqueue_send_task([pid_copy]() {
                replicate_routes_to_peer(pid_copy);
            });
            // And echo a mini.hello so the other side adds us too in
            // case they connected first. Single send — fine inline on the
            // io thread.
            send_mini_hello(pid);
        }
        // (#3 instability fix) g_routes.size() must be read under g_routes_mu
        // — the reaper erases-while-iterating g_routes on another thread, and
        // a lock-free size()/rehash race is UB that can crash the rendezvous
        // node. Snapshot under the lock, exactly like the `status` verb does.
        uint64_t route_count;
        { std::lock_guard<std::mutex> lk(g_routes_mu); route_count = g_routes.size(); }
        nlohmann::json body{{"role",         "mini-node"},
                            {"peer_address", addr},
                            {"route_count",  route_count}};
        r << "{\"req_id\":\"" << req_id << "\",\"status\":\"ok\",\"body\":"
          << body.dump() << "}";
    } else if (type == "mininodes.list") {
        // Return everyone we've identified as a peer mini-node, plus
        // ourselves. The player merges these into its own bootstrap
        // list so it can fail over to a different mini-node without
        // any operator intervention.
        //
        // Every entry now also carries a `load_score` (0..1) so a
        // bootstrapping browser/player can pick the lightest VPS to
        // land on. For peers, the score is whatever they last reported
        // in their mini.hello heartbeat (default 0 = idle/unknown if
        // they're running an older build). For "self" we read our
        // LoadMonitor live.
        nlohmann::json arr = nlohmann::json::array();
        float self_load_score = 0.0f;
        if (g_load_mon) self_load_score = g_load_mon->current().load_score;
        {
            char* own = rats_get_our_peer_id(g_client);
            if (own) {
                arr.push_back({{"rats_peer_id",   own},
                               {"public_address", std::string()},
                               {"self",           true},
                               {"load_score",     self_load_score}});
                rats_string_free(own);
            }
        }
        {
            std::lock_guard<std::mutex> lk(g_mininodes_mu);
            // Cap to a random sample — a player only needs a handful of
            // failover targets, not the entire mesh. Returns everyone at
            // small N, so this matches the old behaviour until the mesh
            // grows past g_mininodes_list_max.
            std::vector<std::string> ids;
            ids.reserve(g_mininode_peers.size());
            for (const auto& p : g_mininode_peers) ids.push_back(p);
            ids = sample_up_to(std::move(ids),
                               static_cast<size_t>(g_mininodes_list_max));
            for (const auto& p : ids) {
                auto addr_it = g_mininode_addr.find(p);
                auto load_it = g_mininode_load.find(p);
                arr.push_back({
                    {"rats_peer_id",   p},
                    {"public_address", addr_it == g_mininode_addr.end()
                                           ? std::string()
                                           : addr_it->second},
                    {"self",           false},
                    {"load_score",     load_it == g_mininode_load.end()
                                           ? 0.0f
                                           : load_it->second},
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
    } else if (type == "chat.create") {
        // Player creates/announces a room. Verify the "mccrm1" envelope,
        // insert into g_chat_rooms (respect the 128 cap), subscribe to the
        // per-room topic, and re-publish the announce to chat:rooms so the
        // rest of the mesh + every player picks it up.
        // Body: {name, topic, creator, creator_pubkey, created_ms, private, sig}
        const auto& b = env.value("body", nlohmann::json::object());
        ChatRoom room;
        room.name       = b.value("name",       std::string());
        room.topic_str  = b.value("topic",      std::string());
        room.creator    = b.value("creator",    std::string());
        room.created_ms = b.value("created_ms", static_cast<uint64_t>(0));
        room.is_private = b.value("private",    false);
        const std::string creator_pubkey = b.value("creator_pubkey", std::string());
        const std::string sig            = b.value("sig",            std::string());
        std::string status = "ok";
        std::string err;
        std::vector<uint8_t> canon;
        if (room.name.empty() || room.creator.empty()) {
            status = "invalid"; err = "missing name/creator";
        } else if (!build_chat_room_canon(room.name, room.creator,
                                          room.created_ms, room.is_private, canon) ||
                   !chat_verify_canon(canon, room.creator, creator_pubkey, sig)) {
            status = "bad_signature"; err = "mccrm1 verify failed";
        } else {
            bool added = false, capped = false;
            {
                std::lock_guard<std::mutex> lk(g_chat_rooms_mu);
                const bool known = g_chat_rooms.find(room.name) != g_chat_rooms.end();
                if (known || g_chat_rooms.size() < kMaxChatRooms) {
                    added = g_chat_rooms.emplace(room.name, room).second;
                } else {
                    capped = true;
                }
            }
            if (capped) {
                status = "room_cap"; err = "chat-room cap reached";
            } else {
                if (added) chat_subscribe_room(g_client, room.name);
                // Re-publish the verbatim signed announce to chat:rooms so
                // the gossip mesh + every other mini-node/player learn it.
                nlohmann::json announce = {
                    {"name",           room.name},
                    {"topic",          room.topic_str},
                    {"creator",        room.creator},
                    {"creator_pubkey", creator_pubkey},
                    {"created_ms",     room.created_ms},
                    {"private",        room.is_private},
                    {"sig",            sig},
                };
                rats_publish_to_topic(g_client, kChatRoomsTopic,
                                      announce.dump().c_str());
            }
        }
        nlohmann::json body{{"name", room.name}, {"error", err}};
        r << "{\"req_id\":\"" << req_id << "\",\"status\":\"" << status
          << "\",\"body\":" << body.dump() << "}";
    } else if (type == "chat.send") {
        // Player sends a signed message into a room. Verify "mccht1",
        // reject banned senders, push to the local ring, publish to the
        // per-room gossip topic, and push live to local watchers.
        // Body: {room, from, from_pubkey, ts_ms, body, sig}
        const auto& b = env.value("body", nlohmann::json::object());
        const std::string room        = b.value("room",        std::string());
        const std::string from        = b.value("from",        std::string());
        const std::string from_pubkey = b.value("from_pubkey", std::string());
        const uint64_t    ts_ms       = b.value("ts_ms",       static_cast<uint64_t>(0));
        const std::string msg_body    = b.value("body",        std::string());
        const std::string sig         = b.value("sig",         std::string());
        std::string status = "ok";
        std::string err;
        std::vector<uint8_t> canon;
        if (room.empty() || from.empty() || ts_ms == 0) {
            status = "invalid"; err = "missing room/from/ts_ms";
        } else if (msg_body.size() > kMaxChatBodyBytes) {
            status = "too_large"; err = "body exceeds 8KB";
        } else if (!build_chat_msg_canon(room, from, ts_ms, msg_body, canon) ||
                   !chat_verify_canon(canon, from, from_pubkey, sig)) {
            status = "bad_signature"; err = "mccht1 verify failed";
        } else {
            bool banned = false;
            {
                std::lock_guard<std::mutex> lk(g_chat_bans_mu);
                auto bit = g_chat_bans.find(room);
                if (bit != g_chat_bans.end() && bit->second.count(from) > 0)
                    banned = true;
            }
            if (banned) {
                status = "banned"; err = "sender is banned from room";
            } else {
                ChatMessage m;
                m.from = from; m.from_pubkey = from_pubkey;
                m.ts_ms = ts_ms; m.body = msg_body; m.sig = sig;
                bool stored = true;
                {
                    std::lock_guard<std::mutex> lk(g_chat_msgs_mu);
                    if (g_chat_msgs.find(room) == g_chat_msgs.end() &&
                        g_chat_msgs.size() >= kMaxChatRooms) {
                        stored = false;
                    } else {
                        auto& dq = g_chat_msgs[room];
                        dq.push_back(m);
                        while (dq.size() > kChatRingPerRoom) dq.pop_front();
                    }
                }
                if (!stored) {
                    status = "room_cap"; err = "chat-room cap reached";
                } else {
                    // Build the verbatim signed envelope (with room) and
                    // publish it to the per-room gossip topic so the mesh +
                    // every other player receives it.
                    nlohmann::json msg_env = {
                        {"room",        room},
                        {"from",        from},
                        {"from_pubkey", from_pubkey},
                        {"ts_ms",       ts_ms},
                        {"body",        msg_body},
                        {"sig",         sig},
                    };
                    const std::string topic = std::string(kChatRoomPrefix) + room;
                    rats_publish_json_to_topic(g_client, topic.c_str(),
                                               msg_env.dump().c_str());
                    // Push live to local watchers (the publish above reaches
                    // remote subscribers; our own watchers need a direct push).
                    chat_push_message_to_watchers(room, msg_env);
                }
            }
        }
        nlohmann::json body{{"room", room}, {"error", err}};
        r << "{\"req_id\":\"" << req_id << "\",\"status\":\"" << status
          << "\",\"body\":" << body.dump() << "}";
    } else if (type == "chat.watch" || type == "chat.unwatch") {
        // Player subscribes/unsubscribes to live pushes for a room. We
        // record the peer_id -> set<room> mapping; on_chat_message and
        // chat.send push to every watcher of the affected room.
        const auto& b = env.value("body", nlohmann::json::object());
        const std::string room = b.value("room", std::string());
        std::string status = "ok";
        std::string err;
        if (room.empty()) {
            status = "invalid"; err = "missing room";
        } else {
            std::lock_guard<std::mutex> lk(g_chat_watchers_mu);
            if (type == "chat.watch") {
                auto& rooms = g_chat_watchers[peer_id];
                if (rooms.size() >= kMaxWatchedRoomsPerPlayer &&
                    rooms.count(room) == 0) {
                    status = "watch_cap"; err = "too many watched rooms";
                    if (rooms.empty()) g_chat_watchers.erase(peer_id);
                } else {
                    rooms.insert(room);
                }
            } else {
                auto it = g_chat_watchers.find(peer_id);
                if (it != g_chat_watchers.end()) {
                    it->second.erase(room);
                    if (it->second.empty()) g_chat_watchers.erase(it);
                }
            }
        }
        nlohmann::json body{{"room", room}, {"error", err}};
        r << "{\"req_id\":\"" << req_id << "\",\"status\":\"" << status
          << "\",\"body\":" << body.dump() << "}";
    } else if (type == "chat.moderate") {
        // Moderation action. Verify "mccmd1", AUTHORIZE if the actor is the
        // room's creator OR is in the cached global-moderator set. For
        // kick_user: ban the target in the room and push "chat.kicked" to
        // them if connected. For remove_room: erase the room + history.
        // Re-publish the signed envelope to chat:mod (sig-dedup) so the mesh
        // converges. Body: {action, room, target, by, by_pubkey, ts_ms, sig}
        chat_refresh_global_mods_if_stale();
        const auto& b = env.value("body", nlohmann::json::object());
        const std::string action     = b.value("action", std::string());
        const std::string room       = b.value("room",   std::string());
        const std::string target     = b.value("target", std::string());
        const std::string by         = b.value("by",     std::string());
        const std::string by_pubkey  = b.value("by_pubkey", std::string());
        const uint64_t    ts_ms      = b.value("ts_ms",  static_cast<uint64_t>(0));
        const std::string sig        = b.value("sig",    std::string());
        std::string status = "ok";
        std::string err;
        std::vector<uint8_t> canon;
        const bool action_ok = (action == "kick_user" || action == "remove_room");
        if (!action_ok || room.empty() || by.empty() || ts_ms == 0) {
            status = "invalid"; err = "bad action/room/by/ts_ms";
        } else if (!build_chat_mod_canon(action, room, target, by, ts_ms, canon) ||
                   !chat_verify_canon(canon, by, by_pubkey, sig)) {
            status = "bad_signature"; err = "mccmd1 verify failed";
        } else {
            // ---- Authorize: creator OR cached global moderator ----------
            bool is_creator = false;
            {
                std::lock_guard<std::mutex> lk(g_chat_rooms_mu);
                auto it = g_chat_rooms.find(room);
                if (it != g_chat_rooms.end())
                    is_creator = (chat_norm_addr(it->second.creator) ==
                                  chat_norm_addr(by));
            }
            bool is_global_mod = false;
            {
                std::lock_guard<std::mutex> lk(g_chat_mods_mu);
                is_global_mod = g_chat_mods.count(chat_norm_addr(by)) > 0;
            }
            if (!is_creator && !is_global_mod) {
                status = "unauthorized";
                err = "actor is neither room creator nor global moderator";
            } else if (action == "kick_user" && target.empty()) {
                status = "invalid"; err = "kick_user needs target";
            } else {
                // ---- Dedup on the signature so a re-broadcast is a no-op --
                bool fresh = false;
                {
                    std::lock_guard<std::mutex> lk(g_chat_mod_seen_mu);
                    if (g_chat_mod_seen.find(sig) == g_chat_mod_seen.end()) {
                        if (g_chat_mod_seen.size() < kMaxChatModSeen)
                            g_chat_mod_seen[sig] = now_ms();
                        fresh = true; // process even if the seen-cap is hit
                    }
                }
                chat_apply_mod_action(action, room, target, by);
                // Re-publish the signed envelope to chat:mod so other
                // mini-nodes apply the same action (only on first sight).
                if (fresh && g_client) {
                    nlohmann::json mod_env = {
                        {"action",    action},
                        {"room",      room},
                        {"target",    target},
                        {"by",        by},
                        {"by_pubkey", by_pubkey},
                        {"ts_ms",     ts_ms},
                        {"sig",       sig},
                    };
                    rats_publish_to_topic(g_client, kChatModTopic,
                                          mod_env.dump().c_str());
                }
            }
        }
        nlohmann::json body{{"room", room}, {"action", action},
                            {"error", err}};
        r << "{\"req_id\":\"" << req_id << "\",\"status\":\"" << status
          << "\",\"body\":" << body.dump() << "}";
    } else if (type == "chat.bans") {
        // Return the current ban list for a room. Body: {room}.
        const auto& b = env.value("body", nlohmann::json::object());
        const std::string room = b.value("room", std::string());
        nlohmann::json arr = nlohmann::json::array();
        if (!room.empty()) {
            std::lock_guard<std::mutex> lk(g_chat_bans_mu);
            auto it = g_chat_bans.find(room);
            if (it != g_chat_bans.end())
                for (const auto& a : it->second) arr.push_back(a);
        }
        r << "{\"req_id\":\"" << req_id << "\",\"status\":\"ok\",\"body\":{"
          << "\"room\":" << nlohmann::json(room).dump()
          << ",\"bans\":" << arr.dump() << "}}";
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
            // (#6) off-io: fan-out to every full node runs on the sender thread.
            std::string pid(peer_id);
            enqueue_send_task([pid]() { broadcast_swarm_peer_online(pid); });
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
        // The mini-node wraps it as a normal bopwire.reply with the
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

        // Store the mapping so we can route the eventual reply back. created_ms
        // is monotonic (#8) for the TTL sweep; target is recorded so
        // on_relay_reply can require the reply to come FROM this peer (#4).
        const std::string fresh = new_relay_req_id();
        {
            std::lock_guard<std::mutex> lk(g_relay_mu);
            g_pending_relays[fresh] = PendingRelay{
                std::string(peer_id), req_id, mono_ms(), target
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
        // Capture rc so we can detect a dead target full node and
        // evict its route immediately — otherwise the player would
        // keep hitting the same offline node for the next TTL window
        // (audit: dead-route eviction on send-failure).
        const auto fwd_rc = rats_send_message(g_client, target.c_str(),
                                              kRequestType, fwd.dump().c_str());
        if (fwd_rc != RATS_SUCCESS) {
            // (#3 pending-entry leak) erase the pending entry we just recorded.
            // The forward never left the box, so no reply can ever match
            // `fresh`; leaving it would let the 1s sweep fire a spurious
            // relay_timeout for the SAME originator we're about to answer with
            // dead_route below (a confusing duplicate fail reply). The req_id is
            // random-seeded now (#4) so it won't be deterministically reused
            // either — removing it explicitly is the only safe option.
            {
                std::lock_guard<std::mutex> lk(g_relay_mu);
                g_pending_relays.erase(fresh);
            }
            // Evict every route entry whose rats_peer_id matches the
            // unreachable target so the player stops hitting the same offline
            // node for the next TTL window.
            {
                std::lock_guard<std::mutex> lk(g_routes_mu);
                for (auto it = g_routes.begin(); it != g_routes.end(); ) {
                    if (it->second.rats_peer_id == target) {
                        const std::string node_id = it->second.node_id;
                        it = g_routes.erase(it);
                        if (!g_quiet) {
                            push_event("route-evict", node_id,
                                       "send_failed");
                        }
                    } else {
                        ++it;
                    }
                }
            }
            // Tell the originator the route is dead so the player can
            // immediately retry against a different full node instead
            // of waiting for a reply that will never come.
            std::ostringstream dr;
            dr << "{\"req_id\":\"" << req_id
               << "\",\"status\":\"dead_route\","
               << "\"error\":\"target unreachable, route evicted\"}";
            send_reply(peer_id, dr.str());
        }
        rats_string_free(peer_id);
        rats_string_free(message_data);
        return; // mini-node sends no immediate reply — wait for the relayed answer
    } else {
        r << "{\"req_id\":\"" << req_id
          << "\",\"status\":\"unknown_type\","
          << "\"error\":\"mini-node only serves routes.get/mininodes.list/mini.hello/status/stun.observe/relay.forward/relay.push.forward/ice.connect_request/chat.list_rooms/chat.history/chat.create/chat.send/chat.watch/chat.unwatch/chat.moderate/chat.bans\"}";
    }

    send_reply(peer_id, r.str());
    rats_string_free(peer_id);
    rats_string_free(message_data);
}

// ---- Relay reply interceptor ----------------------------------------
//
// Catches every `bopwire.reply` directed at the mini-node, checks whether
// the req_id is one we minted while forwarding, and if so routes the reply
// back to the original requester with the original req_id substituted in.

// (#4) A reply only counts when it arrives from the peer we forwarded to.
// An empty recorded target (shouldn't happen for relay.forward entries, but
// be defensive so a missing field doesn't black-hole an otherwise-valid
// reply) accepts any sender. `sender` is the transport peer_id librats handed
// us for this reply.
inline bool rec_target_matches(const std::string& recorded_target,
                               const char* sender) {
    if (recorded_target.empty()) return true;
    return sender && recorded_target == sender;
}

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

    // Our own global-moderator cache refresh reply (fired by
    // chat_refresh_global_mods_if_stale). Absorb it here — it's not a
    // relayed player request, so it must not fall through to the
    // pending-relay path. Body shape: {moderators:[<hex addr>...]}.
    if (req_id.compare(0, 9, "chatmods-") == 0) {
        try {
            const auto& body = env.value("body", nlohmann::json::object());
            if (body.contains("moderators") && body["moderators"].is_array()) {
                std::unordered_set<std::string> mods;
                for (const auto& v : body["moderators"]) {
                    if (!v.is_string()) continue;
                    std::string h = v.get<std::string>();
                    // Normalize to lowercase, strip 0x, so comparisons
                    // against canonical from-address hex are case-stable.
                    if (h.size() >= 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X'))
                        h = h.substr(2);
                    for (auto& c : h) c = static_cast<char>(std::tolower((unsigned char)c));
                    if (h.size() == 40) mods.insert(h);
                }
                std::lock_guard<std::mutex> lk(g_chat_mods_mu);
                g_chat_mods = std::move(mods);
                g_chat_mods_fetched_ms = now_ms();
            }
        } catch (const nlohmann::json::exception&) { /* keep old cache */ }
        rats_string_free(peer_id);
        rats_string_free(message_data);
        return;
    }

    PendingRelay rec;
    bool matched = false;
    {
        std::lock_guard<std::mutex> lk(g_relay_mu);
        auto it = g_pending_relays.find(req_id);
        if (it != g_pending_relays.end()) {
            // (#4 reply correlation) only accept the reply if it actually came
            // from the peer we forwarded TO. An entry with an empty target (a
            // mini.hello / chat / swarm broadcast minted via new_relay_req_id
            // that happens to share the req_id space) or a reply from some other
            // peer must NOT erase + claim this originator's channel. Leave the
            // pending entry in place so the real target's reply can still match.
            if (rec_target_matches(it->second.target_peer_id, peer_id)) {
                rec = it->second;
                g_pending_relays.erase(it);
                matched = true;
            }
        }
    }
    if (!matched) {
        // Not one of our forwarded relay replies (or a mismatched sender) —
        // nothing else claims this channel now that the browser gateway is
        // gone. Drop.
        rats_string_free(peer_id);
        rats_string_free(message_data);
        return;
    }

    env["req_id"] = rec.original_req_id;
    // (#18 instability fix) the originator may have vanished (player
    // reconnected with a fresh transport peer_id) — don't silently
    // black-hole the reply; surface the send result so the dead-originator
    // case is observable rather than a mystery stall. (The player's own
    // re-issue-on-reconnect path is the real recovery.)
    const rats_error_t sent = rats_send_message(
        g_client, rec.originator_peer_id.c_str(),
        kReplyType, env.dump().c_str());
    if (sent != RATS_SUCCESS) {
        if (!g_quiet)
            push_event("relay-rep-drop", rec.originator_peer_id,
                       "send=" + std::to_string((int)sent));
    } else if (!g_quiet) {
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

// (#2 binary NACK) When we can't forward an 'F'-frame — either rats_send_binary
// to the target failed, or the sender's rate-limit bucket is empty — we send a
// tiny control frame straight back to the SENDER so its serving player
// retransmits that one chunk immediately, instead of the receiver waiting out
// its multi-second stream stall for a chunk that never arrives.
//
// Wire format (binary, 9 bytes):
//   byte 0     : 'N' (relay-NACK marker, 0x4E)  — distinct from 'F'
//   bytes 1..4 : stream_id (u32 LE)   } parsed from the FORWARDED payload's
//   bytes 5..8 : seq       (u32 LE)   } librats chunk header (see below)
//
// The forwarded payload (the bytes after our 1+40+16 relay header) starts with
// the librats audio-chunk layout: stream_id LE(4) + seq LE(4) + eof(1) + data.
// CROSS-SUBSYSTEM: the serving player must recognise a 9-byte 'N'-tagged binary
// frame and re-send (stream_id, seq). See the change-list note.
constexpr uint8_t kRelayNackTag = 'N';

inline uint32_t rd_u32_le(const uint8_t* p) {
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

// Emit the NACK back to `sender_pid` for the chunk identified by the first 8
// bytes of `fwd_payload` (stream_id + seq). `fwd_payload_size` must be >= 8 or
// we can't name the chunk and skip (the player will fall back to its timeout).
void send_relay_nack(const std::string& sender_pid,
                     const uint8_t* fwd_payload, size_t fwd_payload_size) {
    if (!g_client || sender_pid.empty() || fwd_payload_size < 8) return;
    uint8_t nack[9];
    nack[0] = kRelayNackTag;
    std::memcpy(nack + 1, fwd_payload, 8); // stream_id(4) + seq(4), verbatim LE
    rats_send_binary(g_client, sender_pid.c_str(), nack, sizeof(nack));
    if (!g_quiet) {
        const uint32_t sid = rd_u32_le(fwd_payload);
        const uint32_t seq = rd_u32_le(fwd_payload + 4);
        push_event("relay-nack", sender_pid,
                   "sid=" + std::to_string(sid) + " seq=" + std::to_string(seq));
    }
}

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
    // Direct audio-stream chunks (no 'F' relay tag) carry the librats
    // wire layout: stream_id LE(4) + seq LE(4) + eof(1) + payload. The
    // audio.fetch bridge tracks stream_id → WS connection in a static
    // registry; let it try to claim the chunk before we treat the
    // buffer as a relay-bridge forward.
    if (size < 1 || b[0] != kRelayBinaryTag) {
        // Non-'F'-tagged binary chunks used to be claimed by the
        // browser audio.fetch handler / WsAudioBridge. With the
        // browser gateway gone, no consumer exists for them — drop.
        cleanup();
        return;
    }
    // #10: extended F-frame is 'F'(1) + target hex(40) + delivery_id(16) +
    // payload. The serving player ALWAYS writes the 16-byte delivery_id on
    // the relay path (zeros if it has none), so we unconditionally strip
    // 1+40+16 — keeping this in lockstep with player_server.dart, or the
    // receiver would read the chunk header off the delivery_id bytes.
    if (size < 1 + 40 + 16) { cleanup(); return; }
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
    uint8_t delivery_id[16];
    std::memcpy(delivery_id, b + 1 + 40, 16);
    const size_t payload_off  = 1 + 40 + 16;
    const size_t payload_size = size - payload_off;
    // ---- Per-peer token-bucket rate limit --------------------------
    //
    // Audit issue: a misbehaving cellular peer could spam relay
    // forwards and saturate the VPS uplink, hurting every other
    // tenant. Charge `size` bytes against the sender's bucket; refill
    // at kRelayRefillBps since last_refill_ms. Drop the frame (do NOT
    // forward) when the bucket would go negative.
    const std::string sender_pid(peer_id);
    bool rate_limited = false;
    {
        const uint64_t now = mono_ms(); // (#8) refill math — monotonic
        std::lock_guard<std::mutex> lk(g_relay_bucket_mu);
        auto [it, inserted] = g_relay_buckets.try_emplace(sender_pid);
        RelayBucket& bk = it->second;
        if (inserted) {
            bk.tokens         = kRelayBucketBytes;
            bk.last_refill_ms = now;
        } else {
            const uint64_t dt_ms = (now > bk.last_refill_ms)
                                       ? (now - bk.last_refill_ms) : 0;
            // refill = bps * dt / 1000, computed without overflow
            const uint64_t refill = (kRelayRefillBps / 1000) * dt_ms;
            bk.tokens = (bk.tokens + refill > kRelayBucketBytes)
                            ? kRelayBucketBytes
                            : (bk.tokens + refill);
            bk.last_refill_ms = now;
        }
        // Charge the PAYLOAD (the bytes that actually traverse the uplink),
        // not the 1+40+16 mini-node header.
        if (bk.tokens < payload_size) {
            rate_limited = true;
        } else {
            bk.tokens -= payload_size;
        }
    }
    // (#2 + #8) Out of tokens. We deliberately do NOT block the io thread to
    // pace this through: librats does accept + recv + decrypt + sends on this
    // ONE thread, so a backpressure sleep here would stall EVERY peer's recv,
    // not just this sender (true pacing is deferred for that reason — see the
    // change-list). Instead of a SILENT discard we NACK the sender keyed by
    // stream_id+seq so its player retransmits that chunk; the chunk is recovered
    // rather than lost, which is the property item #8 actually requires.
    if (rate_limited) {
        if (!g_quiet) {
            push_event("relay-drop", sender_pid, "rate_limited");
        }
        send_relay_nack(sender_pid, b + payload_off, payload_size);
        cleanup();
        return;
    }
    if (!g_quiet) {
        push_event("relay-bin", std::string(target_hex),
                   std::to_string(payload_size) + " B from "
                       + std::string(peer_id).substr(0, 12));
    }
    // (#2 binary NACK) the return code used to be ignored here — a failed
    // forward (target gone, transient send error) left the receiver waiting out
    // its whole stream stall for a chunk that never came. NACK the sender so its
    // player retransmits immediately. We still fall through to the delivery
    // accounting below ONLY on success.
    const rats_error_t bin_rc = rats_send_binary(g_client, target_hex,
                                                 b + payload_off, payload_size);
    if (bin_rc != RATS_SUCCESS) {
        if (!g_quiet) {
            push_event("relay-bin-fail", std::string(target_hex),
                       "rc=" + std::to_string((int)bin_rc));
        }
        send_relay_nack(sender_pid, b + payload_off, payload_size);
        cleanup();
        return;
    }
    // #10: accumulate relayed bytes per delivery_id, keyed by its hex, with
    // the broker (= target full node) so the reaper can flush a signed
    // relay.report. delivery_id == all-zero means "no triangulation" (legacy
    // / direct full-node request) → skip accounting.
    {
        bool all_zero = true;
        for (uint8_t x : delivery_id) if (x) { all_zero = false; break; }
        if (!all_zero) {
            std::string did_hex; did_hex.reserve(32);
            static const char* hx = "0123456789abcdef";
            for (uint8_t x : delivery_id) {
                did_hex.push_back(hx[x >> 4]);
                did_hex.push_back(hx[x & 0xF]);
            }
            std::lock_guard<std::mutex> lk(g_delivery_mu);
            auto& a = g_delivery_accum[did_hex];
            std::memcpy(a.delivery_id.data(), delivery_id, 16);
            a.bytes        += payload_size;
            a.last_ms       = mono_ms(); // (#8) idle-flush clock — monotonic
        }
    }
    cleanup();
}

// ---- Background reaper ---------------------------------------------
//
// #10: flush one delivery accumulator as a signed relay.report to its
// broker (the full node the bytes were relayed to). Signature preimage:
//   "relay.report" || delivery_id(16) || bytes_relayed(u64 LE) || wallet(20)
// signed with the mini-node's own key (whose address == the advertised
// wallet), so the broker verifies recover(sig) == the wallet it knows.
void flush_relay_report(const DeliveryAccum& a) {
    if (!g_client || g_mini_priv.empty()) return;
    std::vector<uint8_t> msg;
    static const char tag[] = "relay.report";
    msg.insert(msg.end(), tag, tag + (sizeof(tag) - 1));        // 12 bytes, no NUL
    msg.insert(msg.end(), a.delivery_id.begin(), a.delivery_id.end());  // 16
    for (int i = 0; i < 8; ++i) msg.push_back(uint8_t(a.bytes >> (8 * i))); // u64 LE
    msg.insert(msg.end(), g_mini_addr20.begin(), g_mini_addr20.end());  // 20
    mc::Sig64 sig = mc::crypto::sign_data(msg.data(), msg.size(), g_mini_priv);
    auto to_hex = [](const uint8_t* p, size_t n){
        static const char* hx = "0123456789abcdef"; std::string s; s.reserve(n*2);
        for (size_t i = 0; i < n; ++i) { s.push_back(hx[p[i]>>4]); s.push_back(hx[p[i]&0xF]); }
        return s; };
    nlohmann::json body = {
        {"delivery_id",   to_hex(a.delivery_id.data(), 16)},
        {"bytes_relayed", a.bytes},
        {"mini_wallet",   g_wallet_address_hex},                    // EIP-55 string
        {"mini_pubkey",   to_hex(g_mini_pub.data(), g_mini_pub.size())}, // 33-byte
        {"sig",           to_hex(sig.data(), sig.size())},          // 64-byte compact
    };
    nlohmann::json env = {
        {"req_id", new_relay_req_id()},
        {"type",   "relay.report"},
        {"body",   body},
    };
    const std::string env_str = env.dump();
    // Broadcast to every full node we know about (g_routes). Only the broker
    // that minted this delivery_id holds the matching pd: row and credits it;
    // every other full node returns "ignored". The rats_peer_id is the dial
    // handle; fall back to node_id when the route didn't carry one.
    std::vector<std::string> targets;
    {
        std::lock_guard<std::mutex> lk(g_routes_mu);
        targets.reserve(g_routes.size());
        for (const auto& kv : g_routes) {
            const std::string& pid = kv.second.rats_peer_id.empty()
                ? kv.second.node_id : kv.second.rats_peer_id;
            if (!pid.empty()) targets.push_back(pid);
        }
    }
    for (const auto& pid : targets)
        rats_send_message(g_client, pid.c_str(), kRequestType, env_str.c_str());
    if (!g_quiet) push_event("relay-report",
                             std::to_string(targets.size()) + " nodes",
                             std::to_string(a.bytes) + "B");
}

// Wakes every kReaperIntervalMs while g_reaper_running. Jobs:
//   1. Expire RouteEntry records older than kRouteTtlMs so a vanished
//      full node stops appearing in routes.get.
//   2. Prune mini-node auxiliary tables (g_mininode_load /
//      g_mininode_addr) for peers whose route just got dropped.
//   3. Prune g_relay_buckets entries for peers no longer present in
//      rats_get_validated_peer_ids — otherwise a churn of cellular
//      peers would let the bucket map grow unboundedly.
//   4. (#10) Flush idle delivery accumulators as signed relay.report.
//   6. (chat) TTL-evict the moderation-action dedup set.
//   7. (chat) refresh the cached global-moderator set.
// NOTE: the pending-relay TTL sweep (formerly job 5) now runs on its own 1s
// timer in pending_sweep_loop (#1) so the fail-fast reply isn't delayed by
// this 30s cadence.
void reaper_loop() {
    while (g_reaper_running.load()) {
        // Sleep in small slices so shutdown is responsive.
        for (int slept = 0;
             slept < kReaperIntervalMs && g_reaper_running.load();
             slept += 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!g_reaper_running.load()) break;

        // (#8) two clocks: mono_now drives every duration/TTL comparison;
        // wall_now is only for the chat-mod dedup TTL whose entries are
        // stamped from the on-the-wire now_ms() (system_clock).
        const uint64_t mono_now = mono_ms();
        const uint64_t wall_now = now_ms();

        // 1 + 2: expire stale routes, take a list of evicted node_ids
        // / rats_peer_ids so we can purge mirror tables outside the
        // routes lock.
        std::vector<std::string> evicted_peers;
        {
            std::lock_guard<std::mutex> lk(g_routes_mu);
            for (auto it = g_routes.begin(); it != g_routes.end(); ) {
                if (it->second.received_at_ms + kRouteTtlMs < mono_now) {
                    if (!it->second.rats_peer_id.empty()) {
                        evicted_peers.push_back(it->second.rats_peer_id);
                    }
                    const std::string node_id = it->second.node_id;
                    it = g_routes.erase(it);
                    if (!g_quiet) {
                        push_event("route-expire", node_id, "ttl");
                    }
                } else {
                    ++it;
                }
            }
        }
        if (!evicted_peers.empty()) {
            std::lock_guard<std::mutex> lk(g_mininodes_mu);
            for (const auto& pid : evicted_peers) {
                g_mininode_load.erase(pid);
                g_mininode_addr.erase(pid);
            }
        }

        // 3: prune buckets for peers that are no longer validated.
        std::unordered_set<std::string> alive;
        if (g_client) {
            int count = 0;
            char** ids = rats_get_validated_peer_ids(g_client, &count);
            if (ids) {
                for (int i = 0; i < count; ++i) {
                    if (ids[i]) {
                        alive.emplace(ids[i]);
                        rats_string_free(ids[i]);
                    }
                }
                std::free(ids);
            }
        }
        {
            std::lock_guard<std::mutex> lk(g_relay_bucket_mu);
            for (auto it = g_relay_buckets.begin();
                 it != g_relay_buckets.end(); ) {
                if (alive.count(it->first) == 0) {
                    it = g_relay_buckets.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // 4: (#10) flush idle delivery accumulators as signed relay.report.
        {
            const uint64_t now = mono_ms(); // (#8) idle math — monotonic
            std::vector<DeliveryAccum> ready;
            {
                std::lock_guard<std::mutex> lk(g_delivery_mu);
                for (auto it = g_delivery_accum.begin();
                     it != g_delivery_accum.end(); ) {
                    if (it->second.last_ms + kDeliveryIdleMs < now) {
                        ready.push_back(it->second);
                        it = g_delivery_accum.erase(it);
                    } else { ++it; }
                }
            }
            for (const auto& a : ready) flush_relay_report(a);
        }

        // 5: (#1 instability fix) the pending-relay sweep used to live here on
        // the 30s reaper cadence, so a synthetic timeout could arrive up to 30s
        // late — long after the player's own 8s/15s timers gave up. It now runs
        // on its own 1s timer (pending_sweep_loop) so the fail-fast reply beats
        // the originator's timer. See kPendingRelayTtlMs.

        // 6: (chat) TTL-evict the moderation-action dedup set so a long-lived
        // mini-node doesn't accumulate every sig it ever saw. Entries older
        // than kChatModSeenTtlMs are well past any realistic re-broadcast.
        {
            std::lock_guard<std::mutex> lk(g_chat_mod_seen_mu);
            for (auto it = g_chat_mod_seen.begin();
                 it != g_chat_mod_seen.end(); ) {
                // chat_mod_seen entries are stamped with now_ms() (wall) so
                // compare against wall_now, not the monotonic clock.
                if (it->second + kChatModSeenTtlMs < wall_now) {
                    it = g_chat_mod_seen.erase(it);
                } else { ++it; }
            }
        }

        // 7: (chat) periodically refresh the cached global-moderator set so
        // chat.moderate can authorize global mods, not just room creators.
        // Internally rate-limited to kChatModsTtlMs.
        chat_refresh_global_mods_if_stale();
    }
}

// ---- Pending-relay sweeper (#1) -------------------------------------
//
// Decoupled from the 30s reaper so the synthetic fail-fast reply arrives
// before the player's own timer. Runs every kPendingSweepIntervalMs, expires
// entries older than kPendingRelayTtlMs (monotonic, #8), and sends each
// stranded originator a `relay_timeout` reply on the original req_id so it
// retries against another full node instead of stalling.
void pending_sweep_loop() {
    while (g_pending_sweep_running.load()) {
        for (int slept = 0;
             slept < kPendingSweepIntervalMs && g_pending_sweep_running.load();
             slept += 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!g_pending_sweep_running.load()) break;

        const uint64_t now = mono_ms();
        struct Expired { std::string originator, orig_req; };
        std::vector<Expired> expired;
        {
            std::lock_guard<std::mutex> lk(g_relay_mu);
            for (auto it = g_pending_relays.begin();
                 it != g_pending_relays.end(); ) {
                if (it->second.created_ms + kPendingRelayTtlMs < now) {
                    expired.push_back({it->second.originator_peer_id,
                                       it->second.original_req_id});
                    it = g_pending_relays.erase(it);
                } else { ++it; }
            }
        }
        for (const auto& e : expired) {
            if (e.originator.empty() || !g_client) continue;
            nlohmann::json err = {
                {"req_id", e.orig_req},
                {"status", "error"},
                {"error",  "relay_timeout"},
            };
            rats_send_message(g_client, e.originator.c_str(),
                              kReplyType, err.dump().c_str());
        }
        if (!expired.empty() && !g_quiet)
            push_event("relay-expire",
                       std::to_string(expired.size()) + " pending", "ttl");
    }
}

// ---- mini.hello heartbeat (#5) --------------------------------------
//
// Load-based VPS selection used to freeze at connect time: g_mininode_load was
// only refreshed when send_mini_hello's reply first arrived. This tick re-sends
// mini.hello to every current mini-node peer every kMiniHelloHeartbeatMs so the
// peer re-reports its load_score and our table tracks real-time load.
constexpr int      kMiniHelloHeartbeatMs = 45 * 1000; // 45s (in 30-60s window)
std::atomic<bool>  g_heartbeat_running{false};
std::thread        g_heartbeat_thread;

void mini_hello_heartbeat_loop() {
    while (g_heartbeat_running.load()) {
        for (int slept = 0;
             slept < kMiniHelloHeartbeatMs && g_heartbeat_running.load();
             slept += 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!g_heartbeat_running.load()) break;

        // Snapshot the peer set under the lock, then send outside it — a single
        // mini.hello per peer is cheap, but we still keep the sends off the lock
        // to avoid holding g_mininodes_mu across rats_send_message.
        std::vector<std::string> peers;
        {
            std::lock_guard<std::mutex> lk(g_mininodes_mu);
            peers.reserve(g_mininode_peers.size());
            for (const auto& p : g_mininode_peers) peers.push_back(p);
        }
        // Sparse mesh: refresh a random g_mesh_fanout-sized slice each round
        // rather than heartbeating every peer (O(N^2) network-wide). Load
        // scores stay approximately fresh via rotation. No-op at small N.
        if (g_sparse_mesh)
            peers = sample_up_to(std::move(peers), static_cast<size_t>(g_mesh_fanout));
        for (const auto& p : peers) send_mini_hello(p);
        if (!peers.empty() && !g_quiet)
            push_event("mini-hello-hb",
                       std::to_string(peers.size()) + " peers", "refresh");
    }
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
        } else if (a == "--sparse-mesh") {
            // Opt into the bounded-degree epidemic gossip overlay instead of
            // the default full mesh. See the g_sparse_mesh comment.
            g_sparse_mesh = true;
        } else if (a == "--mesh-fanout" && i + 1 < argc) {
            g_mesh_fanout = std::atoi(argv[++i]);
        } else if (a == "--mininodes-list-max" && i + 1 < argc) {
            g_mininodes_list_max = std::atoi(argv[++i]);
        } else if (a == "--require-signed-routes") {
            g_require_signed_routes = true;
        } else if (a == "-h" || a == "--help") {
            std::cout << "Usage: mini-node [--rats-port N] [--quiet]\n"
                      << "  --rats-port          librats TCP port (default " << kDefaultRatsPort << ")\n"
                      << "  --quiet              suppress per-peer log lines\n"
                      << "  --peer-vps           host:port[,host:port...] of fellow mini-nodes\n"
                      << "                       to dial at startup so we mesh with them\n"
                      << "  --sparse-mesh        bounded-degree epidemic gossip instead of full mesh\n"
                      << "                       (for thousands of minis; default off)\n"
                      << "  --mesh-fanout N      peers per gossip round when --sparse-mesh (default 16)\n"
                      << "  --mininodes-list-max N  cap mininodes.list to N sampled peers (default 64)\n"
                      << "  --require-signed-routes  drop routes without a valid signature (default off)\n";
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
                    // Sparse-mesh knobs. CLI wins: only take the config value
                    // when the global is still at its compiled-in default
                    // (same idiom as rats_port above).
                    if (!g_sparse_mesh && j.contains("sparse_mesh") &&
                        j["sparse_mesh"].is_boolean())
                        g_sparse_mesh = j["sparse_mesh"].get<bool>();
                    if (g_mesh_fanout == 16 && j.contains("mesh_fanout") &&
                        j["mesh_fanout"].is_number_integer())
                        g_mesh_fanout = j["mesh_fanout"].get<int>();
                    if (g_mininodes_list_max == 64 &&
                        j.contains("mininodes_list_max") &&
                        j["mininodes_list_max"].is_number_integer())
                        g_mininodes_list_max = j["mininodes_list_max"].get<int>();
                    if (!g_require_signed_routes &&
                        j.contains("require_signed_routes") &&
                        j["require_signed_routes"].is_boolean())
                        g_require_signed_routes = j["require_signed_routes"].get<bool>();
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
            j["sparse_mesh"]           = g_sparse_mesh;
            j["mesh_fanout"]           = g_mesh_fanout;
            j["mininodes_list_max"]    = g_mininodes_list_max;
            j["require_signed_routes"] = g_require_signed_routes;
            std::ofstream of(save_path);
            of << j.dump(2);
        } catch (...) {}
    }
    // Clamp the mesh knobs to sane floors so a bad flag/config can't wedge
    // fan-out to zero (which would silently stop route gossip / the list).
    if (g_mesh_fanout < 1)        g_mesh_fanout = 1;
    if (g_mininodes_list_max < 1) g_mininodes_list_max = 1;
    std::cout << "[mini-node] mesh="
              << (g_sparse_mesh ? "sparse" : "full")
              << " fanout=" << g_mesh_fanout
              << " list_max=" << g_mininodes_list_max
              << " require_signed=" << (g_require_signed_routes ? "on" : "off")
              << "\n";

    g_load_mon = std::make_unique<mc::net::LoadMonitor>(load_cfg);
    g_load_mon->start();

    // ---- Wallet identity ---------------------------------------------
    //
    // First-launch: generate a fresh 12-word BIP39 mnemonic and write it
    // to mini-node.seed in the working directory (or the operator-
    // supplied path via $BOPWIRE_MINI_SEED). Re-launch: load whatever
    // is already there. Either way we derive the secp256k1 keypair via
    // the same path the user wallets use (m/44'/19779'/0'/0/0) so the
    // resulting address is portable — an operator can import the
    // mnemonic into MetaMask, ethers.js, the player's wallet flow, etc.
    // and see the same address.
    {
        std::string seed_path = "mini-node.seed";
        if (const char* env = std::getenv("BOPWIRE_MINI_SEED")) {
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
        // #10: retain the keypair so the reaper can sign relay.report.
        g_mini_priv   = kp_opt->private_key;
        g_mini_pub    = kp_opt->public_key;
        g_mini_addr20 = kp_opt->address;
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

    // wallet-as-id: pin the hub's librats peer id to the mini's 20-byte wallet
    // address (lowercase 40-hex) so the relay identity never drifts on rebuild.
    // An unpinned relay id makes the whole swarm unreachable, so fail fast.
    if (g_wallet_address_raw.size() != 40) {
        std::cerr << "[mini-node] FATAL: wallet address is not 40 hex ('"
                  << g_wallet_address_raw
                  << "'); refusing to start with an unpinned relay identity\n";
        return 2;
    }
    rats_client_t client = rats_create_with_id(rats_port, g_wallet_address_raw.c_str());
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

    // Chat moderation: subscribe to the global moderation topic so kick /
    // remove_room actions published by any mini-node converge mesh-wide.
    // on_chat_mod_message verifies + authorizes + applies each action.
    rats_subscribe_to_topic(client, kChatModTopic);
    rats_set_topic_message_callback(client, kChatModTopic,
                                    on_chat_mod_message, nullptr);

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

    // Start the background reaper. It expires stale routes
    // (kRouteTtlMs), trims mini-node mirror tables, and prunes
    // disconnected peers from the relay rate-limit map. Joined on
    // shutdown below.
    g_reaper_running.store(true);
    g_reaper_thread = std::thread(reaper_loop);

    // (#6) Single-consumer sender thread that drains the off-io fan-out work
    // queue (swarm broadcasts, route replication, player replay). Keeps those
    // multi-send loops off the librats io thread. Joined on shutdown below.
    g_sender_running.store(true);
    g_sender_thread = std::thread(sender_loop);

    // (#1) Dedicated 1s pending-relay sweeper — fails stranded relay forwards
    // fast (before the player's own timer) instead of waiting on the 30s reaper.
    g_pending_sweep_running.store(true);
    g_pending_sweep_thread = std::thread(pending_sweep_loop);

    // (#5) mini.hello heartbeat so g_mininode_load tracks real-time peer load
    // for load-based VPS selection instead of freezing at connect time.
    g_heartbeat_running.store(true);
    g_heartbeat_thread = std::thread(mini_hello_heartbeat_loop);

    rats_start_automatic_peer_discovery(client);

    // Browser-facing surfaces (WsTcpRelay, WsAudioBridge, WsMiniGateway)
    // were removed when the web player was dropped — players are now
    // Android + Windows only on librats directly. See memory entries
    // [feedback-no-web-version] and [project-bopwire-mini-node-router].

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
    // Stop + join every background thread before tearing down librats so none
    // can call rats_* on a destroyed client. Order doesn't matter for safety
    // (each only touches its own state + rats_send_*), but stop the sender last
    // so any final fan-outs enqueued by the others can still drain.
    g_reaper_running.store(false);
    if (g_reaper_thread.joinable()) g_reaper_thread.join();
    g_pending_sweep_running.store(false);
    if (g_pending_sweep_thread.joinable()) g_pending_sweep_thread.join();
    g_heartbeat_running.store(false);
    if (g_heartbeat_thread.joinable()) g_heartbeat_thread.join();
    // Sender thread waits on a condvar — clear the flag THEN notify so it wakes,
    // observes !running, and exits.
    g_sender_running.store(false);
    g_send_q_cv.notify_all();
    if (g_sender_thread.joinable()) g_sender_thread.join();
    rats_stop_automatic_peer_discovery(client);
    rats_stop(client);
    rats_destroy(client);
    g_client = nullptr;
    return 0;
}
