#include "sync_manager.h"
#include "../audio/fingerprint.h"
#include "../consensus/candidate.h"  // REQUIRED_CONFIRMATIONS
#include "../crypto/signature.h"
#include "../crypto/hash.h"
#include "../network/rats_link.h"

#include "../../deps/librats/src/librats_c.h"
#include "../../deps/librats/src/json.hpp"

#include <chrono>
#include <iostream>
#include <set>
#include <unordered_set>

namespace mc {

using nlohmann::json;

namespace {
// Same envelope types RatsApi uses. Outbound sync requests go on
// MC_REQUEST_TYPE; replies come back on MC_REPLY_TYPE.
constexpr const char* kMcRequestType = "musicchain.request";
constexpr const char* kMcReplyType   = "musicchain.reply";
} // namespace

SyncManager::SyncManager(Chain& chain, net::RatsLink& rats,
                          uint32_t min_independent_peers)
    : chain_(chain), rats_(rats), min_peers_(min_independent_peers) {}

SyncManager::~SyncManager() { stop(); }

void SyncManager::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread([this] { run_pass(); });
}

void SyncManager::stop() {
    if (!running_.exchange(false)) return;
    // Unblock anybody waiting on an RPC reply so the worker can exit.
    pending_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void SyncManager::set_mini_node_peer_id(const std::string& peer_id) {
    std::lock_guard<std::mutex> lk(discovery_mu_);
    mini_peer_id_ = peer_id;
}

void SyncManager::discover_full_nodes() {
    std::string mini;
    {
        std::lock_guard<std::mutex> lk(discovery_mu_);
        mini = mini_peer_id_;
    }
    if (mini.empty()) return;
    auto reply = rpc(mini, "routes.get", "");
    if (!reply) return;
    try {
        auto body = nlohmann::json::parse(*reply);
        // routes_json builds { "self_load": ..., "wallet": ..., "peers": [ ... ] }
        // but on the home-node side it may be a flat array — handle both.
        const auto& peers = body.contains("peers") ? body["peers"] : body;
        if (!peers.is_array()) return;
        std::vector<DiscoveredFull> next;
        for (const auto& p : peers) {
            if (!p.is_object()) continue;
            DiscoveredFull d;
            d.node_id        = p.value("node_id", std::string{});
            d.rats_peer_id   = p.value("rats_peer_id", std::string{});
            d.public_address = p.value("public_address", std::string{});
            d.reachability   = p.value("reachability", std::string{"unknown"});
            if (d.rats_peer_id.empty()) continue;
            next.push_back(std::move(d));
        }
        std::lock_guard<std::mutex> lk(discovery_mu_);
        discovered_.swap(next);
    } catch (...) { /* keep last known */ }
}

void SyncManager::on_rpc_reply(const std::string& /*peer_id*/,
                                const std::string& reply_json) {
    std::string req_id;
    try {
        auto env = json::parse(reply_json);
        req_id = env.value("req_id", std::string{});
    } catch (...) { return; }
    if (req_id.empty()) return;

    std::lock_guard<std::mutex> lk(pending_mu_);
    auto it = pending_.find(req_id);
    if (it == pending_.end()) return;  // unknown / already-timed-out
    it->second.delivered  = true;
    it->second.reply_json = reply_json;
    pending_cv_.notify_all();
}

std::optional<std::string>
SyncManager::rpc(const std::string& peer_id,
                  const std::string& verb,
                  const std::string& body_json,
                  uint32_t timeout_ms) {
    if (!rats_.client()) return std::nullopt;
    const std::string req_id =
        "sync-" + std::to_string(next_req_id_.fetch_add(1));

    json req = {{"req_id", req_id}, {"type", verb}};
    if (body_json.empty()) {
        req["body"] = json::object();
    } else {
        try { req["body"] = json::parse(body_json); }
        catch (...) { req["body"] = body_json; }
    }
    // Relay wrap: if peer_id is a discovered full node (not the mini-node
    // itself), tunnel the request through the mini-node via relay.forward.
    // The mini-node generates a fresh inner req_id, remembers the
    // originator + our req_id, forwards to the target, and routes the
    // reply back to us with our original req_id intact. The wrap is
    // transparent to the rest of SyncManager.
    std::string send_to_peer = peer_id;
    json        out_req      = req;
    {
        std::lock_guard<std::mutex> lk(discovery_mu_);
        if (!mini_peer_id_.empty() && peer_id != mini_peer_id_) {
            bool is_discovered = false;
            for (const auto& d : discovered_) {
                if (d.rats_peer_id == peer_id) { is_discovered = true; break; }
            }
            if (is_discovered) {
                out_req = {
                    {"req_id", req_id},
                    {"type",   "relay.forward"},
                    {"body",   {
                        {"target_peer_id", peer_id},
                        {"type",           verb},
                        {"body",           req["body"]}
                    }}
                };
                send_to_peer = mini_peer_id_;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_[req_id] = PendingRpc{};
    }
    rats_send_message(rats_.client(), send_to_peer.c_str(),
                      kMcRequestType, out_req.dump().c_str());

    std::unique_lock<std::mutex> lk(pending_mu_);
    const bool got = pending_cv_.wait_for(lk,
        std::chrono::milliseconds(timeout_ms),
        [&]{ return !running_ || pending_[req_id].delivered; });
    auto node = pending_.extract(req_id);
    if (!got || !node || !node.mapped().delivered) return std::nullopt;

    try {
        auto env = json::parse(node.mapped().reply_json);
        if (env.value("status", std::string{}) != "ok") return std::nullopt;
        const auto& body = env["body"];
        return body.is_null() ? std::string{} : body.dump();
    } catch (...) { return std::nullopt; }
}

std::optional<SyncManager::PeerTip>
SyncManager::peer_chain_tip(const std::string& peer_id) {
    auto reply = rpc(peer_id, "chain.tip", "");
    if (!reply) return std::nullopt;
    try {
        auto body = json::parse(*reply);
        PeerTip t{};
        t.height = static_cast<uint32_t>(body.value("height", 0));
        const auto hash_hex = body.value("hash", std::string{});
        auto hb = crypto::from_hex(hash_hex);
        if (hb.size() == 32) std::copy(hb.begin(), hb.end(), t.hash.begin());
        t.timestamp_ms = body.value("timestamp_ms", uint64_t{0});
        return t;
    } catch (...) { return std::nullopt; }
}

std::vector<Hash256>
SyncManager::peer_list_block_hashes(const std::string& peer_id,
                                     uint32_t from_height,
                                     uint32_t max_count) {
    json b = {{"from_height", from_height}, {"max", max_count}};
    auto reply = rpc(peer_id, "chain.list_block_hashes", b.dump());
    std::vector<Hash256> out;
    if (!reply) return out;
    try {
        auto body  = json::parse(*reply);
        const auto& arr = body["hashes"];
        if (!arr.is_array()) return out;
        for (const auto& h_hex : arr) {
            if (!h_hex.is_string()) continue;
            auto bytes = crypto::from_hex(h_hex.get<std::string>());
            if (bytes.size() != 32) continue;
            Hash256 h{};
            std::copy(bytes.begin(), bytes.end(), h.begin());
            out.push_back(h);
        }
    } catch (...) {}
    return out;
}

std::optional<std::vector<uint8_t>>
SyncManager::peer_get_block(const std::string& peer_id, const Hash256& hash) {
    json b = {{"hash", crypto::to_hex(hash)}};
    auto reply = rpc(peer_id, "chain.get_block", b.dump());
    if (!reply) return std::nullopt;
    try {
        auto body = json::parse(*reply);
        const auto b64 = body.value("block_b64", std::string{});
        if (b64.empty()) return std::nullopt;
        return audio::base64_decode(b64);
    } catch (...) { return std::nullopt; }
}

bool SyncManager::ingest_block(const std::vector<uint8_t>& bytes,
                                const Hash256& expected_hash,
                                const Hash256& prev_hash) {
    Block block;
    if (!Block::deserialize(bytes.data(), bytes.size(), block)) {
        std::cerr << "[sync] block deserialize failed\n";
        return false;
    }
    if (block.hash() != expected_hash) {
        std::cerr << "[sync] block hash mismatch — peer substitution?\n";
        return false;
    }
    if (block.header.prev_hash != prev_hash) {
        std::cerr << "[sync] block prev_hash break\n";
        return false;
    }
    if (!block.validate()) {
        std::cerr << "[sync] block.validate failed\n";
        return false;
    }
    // Confirmation quorum check — same as rebuild_derived_state.
    std::set<Hash256> seen;
    uint32_t valid_sigs = 0;
    for (const auto& c : block.header.confirmations) {
        if (!seen.insert(c.validator_id).second) continue;
        if (crypto::verify_ecdsa(block.hash(), c.signature, c.pubkey))
            ++valid_sigs;
    }
    if (valid_sigs < REQUIRED_CONFIRMATIONS) {
        std::cerr << "[sync] block has only " << valid_sigs
                  << " valid confirmations\n";
        return false;
    }
    // Eclipse defense: no contradiction with hardcoded checkpoints.
    const uint32_t height = chain_.tip().height + 1;
    if (!satisfies_checkpoints(height, expected_hash)) {
        std::cerr << "[sync] block contradicts checkpoint at height "
                  << height << "\n";
        return false;
    }
    if (!chain_.connect_block(block)) {
        std::cerr << "[sync] chain.connect_block rejected\n";
        return false;
    }
    return true;
}

bool SyncManager::satisfies_checkpoints(uint32_t height,
                                         const Hash256& hash) const {
    for (const auto& cp : hardcoded_checkpoints()) {
        if (cp.height == height && cp.hash != hash) return false;
    }
    return true;
}

void SyncManager::run_pass() {
    // One full sync pass at startup. Eclipse defense + fork-choice +
    // per-block validation come together here.
    //
    // 1. Collect tips from every validated peer (up to ~1 s of slop).
    // 2. Refuse to sync if fewer than min_peers_ replies came back —
    //    being eclipsed is exactly the "only one peer answers" scenario.
    // 3. Pick the best tip via tip_is_better (longest chain → newest
    //    timestamp → hash-bytewise). Refuse if it's not strictly better
    //    than our local tip.
    // 4. Walk chain.list_block_hashes paginated forward from our tip+1.
    //    For each hash, pull the block, run ingest_block (which runs
    //    every per-block validation). If anything fails, abort.
    // 5. Log the final delta.
    //
    // We hammer ONE peer for the bulk download (the one that gave us
    // the best tip). If they go bad mid-sync, abort and the next pass
    // can pick a different peer. Multi-peer striped download is a
    // future optimisation — sequential is correct + fits a normal
    // boot-time sync window.
    // Retry loop: the first attempt typically fires before the rats
    // handshake to the mini-node completes (so discover_full_nodes()
    // sees 0 peers); subsequent passes recover from new blocks the
    // home node minted while we were idle. We sleep between passes
    // proportionally to whether we found work or not.
    auto sleep_for = [this](int seconds) {
        for (int i = 0; i < seconds && running_; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    };
    while (running_) {
        // Phase 1: ask the mini-node who the other full nodes are.
        discover_full_nodes();
        std::vector<std::string> peer_ids;
        {
            std::lock_guard<std::mutex> lk(discovery_mu_);
            for (const auto& d : discovered_) {
                if (!d.rats_peer_id.empty()) peer_ids.push_back(d.rats_peer_id);
            }
        }
        // Filter ourselves out (we appear in the routes table too).
        {
            const auto own = rats_.rats_peer_id();
            if (!own.empty()) {
                peer_ids.erase(std::remove(peer_ids.begin(), peer_ids.end(), own),
                                peer_ids.end());
            }
        }
        std::cout << "[sync] pass: local height="
                  << chain_.tip().height
                  << ", discovered full nodes=" << peer_ids.size()
                  << ", min_required=" << min_peers_ << "\n";
        if (peer_ids.size() < min_peers_) {
            std::cout << "[sync] not enough peers — retrying in 10s\n";
            sleep_for(10);
            continue;
        }

        struct PeerWithTip { std::string peer_id; PeerTip tip; };
        std::vector<PeerWithTip> tips;
        tips.reserve(peer_ids.size());
        for (const auto& pid : peer_ids) {
            if (!running_) return;
            auto t = peer_chain_tip(pid);
            if (t) tips.push_back({pid, *t});
        }
        if (tips.size() < min_peers_) {
            std::cout << "[sync] " << tips.size()
                      << " peers replied — below min_peers, retrying in 10s\n";
            sleep_for(10);
            continue;
        }

        const auto local = chain_.tip();
        auto as_chain_tip = [](const PeerTip& p) {
            return ChainTip{p.hash, p.height, p.timestamp_ms};
        };
        PeerWithTip best = tips.front();
        for (const auto& p : tips) {
            if (tip_is_better(as_chain_tip(p.tip), as_chain_tip(best.tip)))
                best = p;
        }
        if (!tip_is_better(as_chain_tip(best.tip), local)) {
            std::cout << "[sync] local tip already ≥ best peer tip — sleeping 30s\n";
            sleep_for(30);
            continue;
        }
        std::cout << "[sync] best peer tip height=" << best.tip.height
                  << " ts=" << best.tip.timestamp_ms
                  << " peer=" << best.peer_id.substr(0, 12) << "…\n";

        Hash256 prev_hash = local.hash;
        uint32_t cursor   = local.height + 1;
        bool aborted = false;
        while (running_ && cursor <= best.tip.height) {
            auto hashes = peer_list_block_hashes(best.peer_id, cursor, 128);
            if (hashes.empty()) {
                std::cerr << "[sync] peer returned no hashes at " << cursor
                          << " — aborting pass\n";
                aborted = true;
                break;
            }
            for (const auto& h : hashes) {
                if (!running_) return;
                auto bytes = peer_get_block(best.peer_id, h);
                if (!bytes) {
                    std::cerr << "[sync] failed to fetch block " << cursor
                              << " — aborting pass\n";
                    aborted = true;
                    break;
                }
                if (!ingest_block(*bytes, h, prev_hash)) {
                    std::cerr << "[sync] ingest_block failed at " << cursor
                              << " — aborting pass\n";
                    aborted = true;
                    break;
                }
                prev_hash = h;
                ++cursor;
                if (cursor > best.tip.height) break;
            }
            if (aborted) break;
        }
        if (aborted) {
            sleep_for(10);
            continue;
        }
        std::cout << "[sync] caught up to height " << chain_.tip().height
                  << " — sleeping 30s before next pass\n";
        sleep_for(30);
    }
}

} // namespace mc
