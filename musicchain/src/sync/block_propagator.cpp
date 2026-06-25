#include "block_propagator.h"

#include "../audio/fingerprint.h"      // base64
#include "../crypto/hash.h"            // to_hex / parse_hash256
#include "../network/rats_link.h"

#include "../../deps/librats/src/librats_c.h"

#include <nlohmann/json.hpp>
#include <openssl/sha.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace mc {

using nlohmann::json;

namespace {
constexpr const char* kReqType = "musicchain.request";

std::string make_req_id() {
    static std::atomic<uint64_t> counter{0};
    return "bp-" + std::to_string(counter.fetch_add(1));
}

} // anonymous

// =====================================================================
// ctor / sha1 helpers
// =====================================================================

BlockPropagator::BlockPropagator(Chain& chain,
                                  net::RatsLink& rats,
                                  const net::NodeConfig& cfg)
    : chain_(chain), rats_(rats), cfg_(cfg) {
    if (cfg_.dht_bootstrap_hash.empty()) {
        bootstrap_key_ = sha1_hex("musicchain-fullnode-mainnet");
    } else {
        bootstrap_key_ = cfg_.dht_bootstrap_hash;
    }
}

BlockPropagator::~BlockPropagator() { stop(); }

std::string BlockPropagator::sha1_hex(const void* data, size_t n) {
    unsigned char out[SHA_DIGEST_LENGTH];
    SHA1(static_cast<const unsigned char*>(data), n, out);
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (int i = 0; i < SHA_DIGEST_LENGTH; ++i)
        os << std::setw(2) << static_cast<int>(out[i]);
    return os.str();
}

std::string BlockPropagator::dht_key_for_block(const Hash256& h) {
    return sha1_hex(h.data(), h.size());
}

// =====================================================================
// start / stop
// =====================================================================

void BlockPropagator::start(rats_client_t client) {
    if (running_.exchange(true)) return;
    client_ = client;
    if (!client_) {
        std::cerr << "[bp] start: null rats client\n";
        running_ = false;
        return;
    }

    if (!rats_is_dht_running(client_)) {
        if (rats_start_dht_discovery(client_, 0) != 0) {
            std::cerr << "[bp] rats_start_dht_discovery failed; continuing without DHT\n";
        }
    }

    // Path A bootstrap: announce + find under the full-node marker so
    // peers naturally find each other without a hardcoded VPS.
    dht_announce(bootstrap_key_);
    std::cout << "[bp] DHT bootstrap key " << bootstrap_key_ << "\n";

    announce_thread_ = std::thread([this] { dht_announce_loop(); });
    apply_thread_    = std::thread([this] { apply_loop(); });
    stall_thread_    = std::thread([this] { stall_loop(); });
    seed_thread_     = std::thread([this] { seed_dial_loop(); });
}

void BlockPropagator::stop() {
    if (!running_.exchange(false)) return;
    apply_cv_.notify_all();
    if (announce_thread_.joinable()) announce_thread_.join();
    if (apply_thread_.joinable())    apply_thread_.join();
    if (stall_thread_.joinable())    stall_thread_.join();
    if (seed_thread_.joinable())     seed_thread_.join();
    client_ = nullptr;
}

// =====================================================================
// DHT — announce + on_peers_found
// =====================================================================

void BlockPropagator::dht_announce(const std::string& key_hex) {
    if (!client_) return;
    rats_announce_for_hash(client_,
                            key_hex.c_str(),
                            static_cast<int>(cfg_.rats_port),
                            &BlockPropagator::on_peers_found_cb,
                            this);
}

void BlockPropagator::on_peers_found_cb(void* user_data,
                                         const char** peer_addresses,
                                         int count) {
    auto* self = static_cast<BlockPropagator*>(user_data);
    if (!peer_addresses) return;
    for (int i = 0; i < count; ++i) {
        const char* addr = peer_addresses[i];
        if (addr && self && self->client_) {
            std::string s(addr);
            auto colon = s.find_last_of(':');
            if (colon != std::string::npos) {
                int port = 0;
                try { port = std::stoi(s.substr(colon + 1)); } catch (...) {}
                if (port > 0 && port < 65536) {
                    // librats dedupes against in-flight + established
                    // connects, so spamming is cheap.
                    rats_connect(self->client_,
                                 s.substr(0, colon).c_str(),
                                 port);
                }
            }
        }
        // musicchain librats patch: caller owns + frees each entry.
        if (addr) std::free(const_cast<char*>(addr));
    }
    std::free(const_cast<char**>(peer_addresses));
}

void BlockPropagator::announce_new_block(const Hash256& hash) {
    // Connectivity gate. Only fan out when we have a confirmed full-node peer.
    // While isolated (count == 0) HOLD the hash and defer BOTH INV gossip and
    // the DHT-announce: broadcasting into the void is pointless, and an
    // isolated node that minted a fork shouldn't shout it (it would only emit
    // INV that gets reorged away on rejoin). Minting is NEVER gated — the block
    // is already in our chain (connect_block ran before this), so a lone
    // bootstrap node still produces + serves and just flushes everything held
    // the instant a full-node peer appears (here, or in the node.hello handler).
    if (full_node_peer_count() == 0) {
        std::lock_guard<std::mutex> lk(pending_mu_);
        if (pending_announce_.size() < kPendingAnnounceCap)
            pending_announce_.push_back(hash);
        return;
    }
    flush_pending_announce();   // catch peers up on anything minted while alone
    do_announce(hash);
}

size_t BlockPropagator::full_node_peer_count() {
    std::lock_guard<std::mutex> lk(peers_mu_);
    size_t n = 0;
    for (const auto& kv : peers_) if (kv.second.is_full_node) ++n;
    return n;
}

void BlockPropagator::flush_pending_announce() {
    std::vector<Hash256> held;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        if (pending_announce_.empty()) return;
        held.assign(pending_announce_.begin(), pending_announce_.end());
        pending_announce_.clear();
    }
    for (const auto& h : held) do_announce(h);
}

void BlockPropagator::do_announce(const Hash256& hash) {
    // 1. INV broadcast to every connected librats peer that hasn't
    //    already told us they know the hash. This is the live
    //    propagation path -- DHT-announce takes seconds.
    if (client_) {
        std::vector<std::string> targets;
        {
            std::lock_guard<std::mutex> lk(peers_mu_);
            for (auto& [pid, ps] : peers_) {
                if (ps.known.find(hash) == ps.known.end()) {
                    targets.push_back(pid);
                    ps.known.insert(hash);
                }
            }
        }
        if (!targets.empty()) {
            const std::string hash_hex = crypto::to_hex(hash);
            json body = {{"hashes", json::array({hash_hex})}};
            for (const auto& pid : targets) {
                send_request(pid, "block.inv", body);
            }
        }
    }

    // 2. DHT-announce the block hash so multi-source catch-up (path B)
    //    can find this node. announced_ dedupes so dht_announce_loop's
    //    initial-chain sweep doesn't double-announce.
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        announced_.insert(hash);
    }
    dht_announce(dht_key_for_block(hash));
}

// =====================================================================
// Peer lifecycle
// =====================================================================

void BlockPropagator::on_peer_connected(const std::string& peer_id) {
    {
        std::lock_guard<std::mutex> lk(peers_mu_);
        peers_.try_emplace(peer_id);
        // (#11 instability fix) per-peer block.hello cooldown. A flapping
        // peer reconnecting would otherwise re-trigger block.hello →
        // getblocks catch-up on every connect. Prune stale entries while
        // here so last_hello_at_ stays bounded by recently-active peers.
        const auto now = std::chrono::steady_clock::now();
        for (auto it = last_hello_at_.begin(); it != last_hello_at_.end(); ) {
            if (now - it->second > kHelloCooldown) it = last_hello_at_.erase(it);
            else ++it;
        }
        auto it = last_hello_at_.find(peer_id);
        if (it != last_hello_at_.end() && now - it->second < kHelloCooldown) {
            return;   // said hello to this peer too recently — skip
        }
        last_hello_at_[peer_id] = now;
    }
    // Fire block.hello with our tip. Reply body will be peer's tip;
    // handle_request("block.hello") on the wire path records it and
    // kicks off block.getblocks if we're behind.
    const auto t = chain_.tip();
    json body = {
        {"tip_height",   t.height},
        {"tip_hash",     crypto::to_hex(t.hash)},
        {"timestamp_ms", t.timestamp_ms},
        {"tip_weight",   t.weight},   // #8: cumulative audited plays
    };
    send_request(peer_id, "block.hello", body);

    // node.hello — announce our full-node identity/role right after block.hello
    // (same cooldown gate above). The peer records role=="full-node" so it
    // counts us toward its connectivity gate; node_id binds our transport id to
    // our chain identity. Reply is advisory; mutual registration is guaranteed
    // because both sides send this on connect. (Signature is deferred:
    // record-but-don't-reject for now — see fingerprint of the rollout plan.)
    json nh_body = {
        {"role",       "full-node"},
        {"node_id",    crypto::to_hex(cfg_.node_id)},
        {"tip_height", t.height},
        {"tip_hash",   crypto::to_hex(t.hash)},
        {"tip_weight", t.weight},
    };
    send_request(peer_id, "block.node_hello", nh_body);
}

void BlockPropagator::on_peer_disconnected(const std::string& peer_id) {
    // (#9 instability fix) re-dispatch the dropped peer's in-flight getdata
    // hashes to a surviving peer. Without this, blocks we'd asked the dead
    // peer for stall until an unrelated re-INV happens to re-trigger them,
    // wedging block sync. Collect under the lock, erase, then re-schedule
    // OUTSIDE the lock (schedule_getdata takes peers_mu_ itself — calling it
    // while held would deadlock), mirroring stall_loop's pattern.
    std::vector<Hash256> orphaned;
    {
        std::lock_guard<std::mutex> lk(peers_mu_);
        auto it = peers_.find(peer_id);
        if (it != peers_.end()) {
            orphaned.assign(it->second.in_flight.begin(),
                            it->second.in_flight.end());
            peers_.erase(it);
        }
        last_hello_at_.erase(peer_id);
    }
    for (const auto& h : orphaned) schedule_getdata(h);
}

// =====================================================================
// send_request (envelope build + librats fire)
// =====================================================================

void BlockPropagator::send_request(const std::string& peer_id,
                                    const std::string& type,
                                    const json& body) {
    if (!client_ || peer_id.empty()) return;
    json env = {
        {"req_id", make_req_id()},
        {"type",   type},
        {"body",   body},
    };
    const std::string payload = env.dump();
    rats_send_message(client_, peer_id.c_str(), kReqType, payload.c_str());
}

// =====================================================================
// handle_request — inbound dispatch
// =====================================================================

nlohmann::json BlockPropagator::handle_request(const std::string& peer_id,
                                                const std::string& type,
                                                const json& body) {
    if (type == "block.hello") {
        // Record peer tip. If we're behind, kick getblocks. If they're
        // behind, our reply body lets them do the same.
        uint64_t peer_weight = 0;
        uint32_t peer_height = 0;
        {
            std::lock_guard<std::mutex> lk(peers_mu_);
            auto& ps = peers_[peer_id];
            ps.tip_height = static_cast<uint32_t>(body.value("tip_height", 0));
            ps.tip_weight = body.value("tip_weight", static_cast<uint64_t>(0));
            const std::string hh = body.value("tip_hash", std::string{});
            crypto::parse_hash256(hh, ps.tip_hash);
            ps.hello_received = true;
            // Snapshot under the lock — do NOT hold a pointer into peers_
            // past the unlock (on_peer_disconnected may erase this entry on
            // another thread, dangling the pointer).
            peer_weight = ps.tip_weight;
            peer_height = ps.tip_height;
        }
        const auto local = chain_.tip();
        // #8: sync toward a peer with a *better* tip by the fork-choice
        // rule (heavier wins; height is only the tiebreak), not just a
        // taller one — a heavier-but-shorter chain must still pull us.
        const bool peer_ahead =
            peer_weight > local.weight ||
            (peer_weight == local.weight && peer_height > local.height);
        if (peer_ahead) {
            json gb_body = {{"locator", json::array()}};
            for (const auto& h : build_locator())
                gb_body["locator"].push_back(crypto::to_hex(h));
            send_request(peer_id, "block.getblocks", gb_body);
        }
        // Reply with our own tip — saves a round trip on the symmetric
        // handshake (each side learns the other's tip from one
        // exchange).
        return {
            {"tip_height",   local.height},
            {"tip_hash",     crypto::to_hex(local.hash)},
            {"timestamp_ms", local.timestamp_ms},
            {"tip_weight",   local.weight},
        };
    }

    if (type == "block.node_hello") {
        // Full-node identity/role handshake. Record role + node_id into the
        // peer entry; is_full_node is what the connectivity gate counts.
        // Signature verification is intentionally deferred (record-but-don't-
        // reject) so this can roll out without bricking the live node — a peer
        // that spoofs role=full-node only causes us to gossip to a non-serving
        // peer (harmless). Turn on ECDSA verification (node_id<->pubkey bind)
        // before relying on the count for anything beyond the announce gate.
        const std::string role = body.value("role",    std::string{});
        const std::string nid  = body.value("node_id", std::string{});
        bool became_full_node = false;
        if (role == "full-node") {
            std::lock_guard<std::mutex> lk(peers_mu_);
            auto& ps = peers_[peer_id];
            became_full_node = !ps.is_full_node;
            ps.is_full_node  = true;
            ps.node_id       = nid;
            ps.node_hello_at = std::chrono::steady_clock::now();
        }
        // On the 0->1 (or any new full-node-peer) transition, flush blocks we
        // minted while isolated so the peer catches up immediately. Done OUTSIDE
        // peers_mu_ — flush_pending_announce + do_announce take peers_mu_ again.
        if (became_full_node) flush_pending_announce();
        // Symmetric reply with our identity (advisory; the sender ignores it,
        // exactly like block.hello / mini.hello).
        return {
            {"role",     "full-node"},
            {"node_id",  crypto::to_hex(cfg_.node_id)},
            {"accepted", true},
        };
    }

    if (type == "block.getblocks") {
        std::vector<Hash256> locator;
        if (body.contains("locator") && body["locator"].is_array()) {
            for (const auto& h_hex : body["locator"]) {
                if (!h_hex.is_string()) continue;
                Hash256 h{};
                if (crypto::parse_hash256(h_hex.get<std::string>(), h))
                    locator.push_back(h);
            }
        }
        const auto out = hashes_after_locator(locator);
        std::cerr << "[bp] block.getblocks from " << peer_id.substr(0, 12)
                  << " locator_size=" << locator.size()
                  << " returning_hashes=" << out.size()
                  << " our_tip=" << chain_.tip().height << "\n";
        // Send the hashes back as a separate block.inv push (NOT in the
        // reply body — the rats_api reply lane is single-shot and the
        // requester's BlockPropagator only listens for inbound
        // musicchain.requests, not replies). Receiver treats the
        // resulting inv the same as any other live announce.
        if (!out.empty()) {
            json inv_body = {{"hashes", json::array()}};
            for (const auto& h : out) inv_body["hashes"].push_back(crypto::to_hex(h));
            send_request(peer_id, "block.inv", inv_body);
            std::cerr << "[bp] sent block.inv to " << peer_id.substr(0, 12)
                      << " with " << out.size() << " hashes\n";
        }
        return json::object();
    }

    if (type == "block.inv") {
        // Peer announces blocks they know about. Each hash we don't
        // have goes into expected_sequence_ (so apply_loop will
        // process when it arrives) and triggers a getdata.
        if (!body.contains("hashes") || !body["hashes"].is_array()) return json::object();
        std::vector<Hash256> news;
        for (const auto& h_hex : body["hashes"]) {
            if (!h_hex.is_string()) continue;
            Hash256 h{};
            if (!crypto::parse_hash256(h_hex.get<std::string>(), h)) continue;
            if (chain_.get_block_height(h).has_value()) continue; // already have it
            news.push_back(h);
        }
        if (!news.empty()) {
            // Peer knows these, so don't re-INV them back.
            {
                std::lock_guard<std::mutex> lk(peers_mu_);
                auto& ps = peers_[peer_id];
                for (const auto& h : news) ps.known.insert(h);
            }
            {
                std::lock_guard<std::mutex> lk(pending_mu_);
                for (const auto& h : news) {
                    // Skip if already queued.
                    if (std::find(expected_sequence_.begin(),
                                  expected_sequence_.end(), h)
                            == expected_sequence_.end())
                    {
                        expected_sequence_.push_back(h);
                    }
                }
            }
            for (const auto& h : news) schedule_getdata(h);
        }
        return json::object();
    }

    if (type == "block.getdata") {
        // Peer asked for specific blocks. We respond with one block.data
        // per accepted hash (chunked, per the chunk-it-now requirement)
        // and a single reply listing accepted + notfound.
        json accepted = json::array();
        json notfound = json::array();
        if (body.contains("hashes") && body["hashes"].is_array()) {
            uint32_t served = 0;
            for (const auto& h_hex : body["hashes"]) {
                if (!h_hex.is_string()) continue;
                if (served >= kMaxInvCount) {
                    notfound.push_back(h_hex);   // request too big; tell peer to re-ask
                    continue;
                }
                Hash256 h{};
                if (!crypto::parse_hash256(h_hex.get<std::string>(), h)) {
                    notfound.push_back(h_hex);
                    continue;
                }
                auto blk = chain_.get_block(h);
                if (!blk) {
                    notfound.push_back(h_hex);
                    continue;
                }
                const auto bytes = blk->serialize();
                json data_body = {
                    {"hash",      h_hex},
                    {"bytes_b64", audio::base64_encode(bytes.data(),
                                                       bytes.size())},
                };
                send_request(peer_id, "block.data", data_body);
                accepted.push_back(h_hex);
                ++served;
            }
        }
        return {{"accepted", accepted}, {"notfound", notfound}};
    }

    if (type == "block.data") {
        const std::string h_hex = body.value("hash", std::string{});
        const std::string b64   = body.value("bytes_b64", std::string{});
        Hash256 h{};
        if (!crypto::parse_hash256(h_hex, h) || b64.empty()) {
            return json::object();
        }
        auto bytes = audio::base64_decode(b64);
        if (bytes.empty()) return json::object();

        // Clear peer.in_flight entry — they delivered.
        {
            std::lock_guard<std::mutex> lk(peers_mu_);
            auto it = peers_.find(peer_id);
            if (it != peers_.end()) it->second.in_flight.erase(h);
        }

        // Stash for the apply_loop. Validation and connect_block happen
        // there so blocks always land in chain order (prev_hash chains
        // correctly even when getdata fans out across peers).
        ingest_block_bytes(bytes, h);
        return json::object();
    }

    // Unknown block.* verb.
    return {{"error", "unknown_block_verb"}};
}

// =====================================================================
// build_locator / hashes_after_locator
// =====================================================================

std::vector<Hash256> BlockPropagator::build_locator() const {
    std::vector<Hash256> out;
    const auto tip = chain_.tip();
    if (tip.height == 0 && tip.hash == Hash256{}) return out;
    uint32_t step = 1;
    int64_t  h    = static_cast<int64_t>(tip.height);
    while (h >= 0) {
        if (auto bh = chain_.get_block_hash(static_cast<uint32_t>(h)))
            out.push_back(*bh);
        if (out.size() >= 10) step *= 2;
        h -= step;
    }
    // Always include genesis at the tail (if not already there).
    if (auto g = chain_.get_block_hash(0)) {
        if (out.empty() || out.back() != *g) out.push_back(*g);
    }
    return out;
}

std::vector<Hash256> BlockPropagator::hashes_after_locator(
        const std::vector<Hash256>& locator) const {
    const auto tip = chain_.tip();
    uint32_t fork_height = 0;
    bool     found_fork  = false;
    for (const auto& h : locator) {
        auto hh = chain_.get_block_height(h);
        if (hh.has_value()) { fork_height = *hh; found_fork = true; break; }
    }
    std::vector<Hash256> out;
    if (!found_fork) {
        // Empty locator OR peer has nothing in common with our chain
        // (the brand-new VPS case — its tip is the zero hash, so
        // build_locator returns []). Send hashes from height 1
        // (genesis) onward — heights are 1-indexed; height 0 has no
        // block, so starting at 0 would `else break` on the first
        // iteration and yield an empty list.
        for (uint32_t hi = 1;
             hi <= tip.height && out.size() < kMaxInvCount;
             ++hi) {
            if (auto bh = chain_.get_block_hash(hi)) out.push_back(*bh);
            else break;
        }
        return out;
    }
    for (uint32_t hi = fork_height + 1;
         hi <= tip.height && out.size() < kMaxInvCount;
         ++hi) {
        if (auto bh = chain_.get_block_hash(hi)) out.push_back(*bh);
        else break;
    }
    return out;
}

// =====================================================================
// ingest + apply_loop
// =====================================================================

bool BlockPropagator::ingest_block_bytes(const std::vector<uint8_t>& bytes,
                                          const Hash256& expected_hash) {
    Block block;
    if (!Block::deserialize(bytes.data(), bytes.size(), block)) {
        std::cerr << "[bp] deserialize failed for "
                  << crypto::to_hex(expected_hash).substr(0, 12) << "…\n";
        return false;
    }
    if (block.hash() != expected_hash) {
        std::cerr << "[bp] block hash mismatch — discarding\n";
        return false;
    }
    if (!block.validate()) {
        std::cerr << "[bp] block.validate failed\n";
        return false;
    }

    // Model 1 (vote-free deterministic consensus): there are no block-
    // level confirmations to verify here. Validity is purely the
    // deterministic content check — block.validate() above (fingerprint/
    // merkle commitment) plus the prev_hash chain link and
    // apply_transactions, both enforced when apply_loop hands the block
    // to chain.connect_block. Every node re-derives the same verdict, so
    // a forged block dies at the first honest hop. (Transactions keep
    // their own signatures; only block-level voting was removed.)

    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_blocks_.emplace(expected_hash, std::move(block));
        // If this hash wasn't in expected_sequence_ (e.g. INV came
        // straight to block.data without prior queueing), append it
        // so apply_loop will pick it up.
        if (std::find(expected_sequence_.begin(),
                      expected_sequence_.end(),
                      expected_hash) == expected_sequence_.end())
        {
            expected_sequence_.push_back(expected_hash);
        }
    }
    apply_cv_.notify_one();
    return true;
}

void BlockPropagator::apply_loop() {
    while (running_) {
        std::unique_lock<std::mutex> lk(pending_mu_);
        apply_cv_.wait_for(lk, std::chrono::seconds(5),
            [&] { return !running_ || !pending_blocks_.empty(); });
        if (!running_) return;

        bool did_apply = false;
        while (!expected_sequence_.empty()) {
            const Hash256 next = expected_sequence_.front();
            auto it = pending_blocks_.find(next);
            if (it == pending_blocks_.end()) break;
            Block blk = std::move(it->second);
            pending_blocks_.erase(it);
            expected_sequence_.pop_front();
            lk.unlock();

            // prev_hash chain check. If blk extends our tip, connect it.
            // If it forks (prev_hash != tip), try to assemble the branch
            // and adopt it iff it's heavier (#8 fork choice).
            const auto local = chain_.tip();
            if (blk.header.prev_hash == local.hash) {
                if (chain_.connect_block(blk)) {
                    did_apply = true;
                    std::cout << "[bp] connected block at height "
                              << chain_.tip().height
                              << " hash="
                              << crypto::to_hex(blk.hash()).substr(0, 12)
                              << "…\n";
                    announce_new_block(blk.hash());   // gossip onward
                } else {
                    std::cerr << "[bp] connect_block rejected\n";
                }
            } else {
                // FORK. Walk back from blk toward a block already on our
                // chain, pulling intermediate fork blocks from the pending
                // buffer, to build a contiguous branch from the fork point.
                std::vector<Block> rev;            // newest-first
                rev.push_back(blk);
                Hash256  want = blk.header.prev_hash;
                bool     found_fork = false;
                Hash256  fork_hash{};
                uint32_t fork_height = 0;
                for (int guard = 0; guard < 100000; ++guard) {
                    if (auto fh = chain_.get_block_height(want)) {
                        fork_hash = want; fork_height = *fh; found_fork = true;
                        break;
                    }
                    std::lock_guard<std::mutex> pk(pending_mu_);
                    auto pit = pending_blocks_.find(want);
                    if (pit == pending_blocks_.end()) break;  // ancestor missing
                    rev.push_back(pit->second);
                    want = pit->second.header.prev_hash;
                }
                if (found_fork) {
                    std::vector<Block> branch(rev.rbegin(), rev.rend());
                    std::string rerr;
                    if (chain_.reorg_to_branch(fork_hash, fork_height,
                                               branch, rerr)) {
                        did_apply = true;
                        {   // drop the consumed fork blocks from the buffer
                            std::lock_guard<std::mutex> pk(pending_mu_);
                            for (const auto& b : branch)
                                pending_blocks_.erase(b.hash());
                        }
                        announce_new_block(chain_.tip().hash);
                        std::cout << "[bp] reorged to height "
                                  << chain_.tip().height << "\n";
                    } else {
                        // Not heavier (or failed) — leave our chain as is.
                        std::cerr << "[bp] fork not adopted: " << rerr << "\n";
                    }
                } else {
                    std::cerr << "[bp] fork at height "
                              << (local.height + 1)
                              << " — branch incomplete, dropping for now\n";
                }
            }
            lk.lock();
        }

        // Continuation: if any peer is still ahead of us and we have
        // nothing pending for them, send another getblocks. This drives
        // multi-batch catch-up to completion without external nudges.
        if (did_apply || expected_sequence_.empty()) {
            const auto local = chain_.tip();
            std::vector<std::string> followups;
            {
                std::lock_guard<std::mutex> pk(peers_mu_);
                for (auto& [pid, ps] : peers_) {
                    // #8: "ahead" = better by fork weight, height as tiebreak.
                    const bool ahead =
                        ps.tip_weight > local.weight ||
                        (ps.tip_weight == local.weight &&
                         ps.tip_height  > local.height);
                    if (ps.hello_received && ahead && ps.in_flight.empty())
                        followups.push_back(pid);
                }
            }
            lk.unlock();
            if (!followups.empty()) {
                json gb_body = {{"locator", json::array()}};
                for (const auto& h : build_locator())
                    gb_body["locator"].push_back(crypto::to_hex(h));
                for (const auto& pid : followups) {
                    send_request(pid, "block.getblocks", gb_body);
                }
            }
            lk.lock();
        }
    }
}

// =====================================================================
// schedule_getdata — picks a peer (or fires DHT-find)
// =====================================================================

void BlockPropagator::schedule_getdata(const Hash256& hash) {
    if (chain_.get_block_height(hash).has_value()) return;
    std::string send_to;
    {
        std::lock_guard<std::mutex> lk(peers_mu_);
        // Prefer a peer that already knows this hash AND has room.
        for (auto& [pid, ps] : peers_) {
            if (ps.in_flight.find(hash) != ps.in_flight.end()) {
                // Already in flight to someone — leave it alone.
                return;
            }
            if (ps.known.find(hash) != ps.known.end() &&
                ps.in_flight.size() < kMaxGetdataInFlight)
            {
                send_to = pid;
                ps.in_flight.insert(hash);
                ps.in_flight_since = std::chrono::steady_clock::now();
                break;
            }
        }
        if (send_to.empty()) {
            // No connected peer claims to have it. Pick whichever
            // connected peer has the most slack and ask them anyway
            // (they may have it without having told us via inv).
            std::string best;
            size_t best_load = kMaxGetdataInFlight;
            for (auto& [pid, ps] : peers_) {
                if (ps.in_flight.size() < best_load) {
                    best = pid;
                    best_load = ps.in_flight.size();
                }
            }
            if (!best.empty()) {
                auto& ps = peers_[best];
                send_to = best;
                ps.in_flight.insert(hash);
                ps.in_flight_since = std::chrono::steady_clock::now();
            }
        }
    }

    if (!send_to.empty()) {
        json body = {{"hashes", json::array({crypto::to_hex(hash)})}};
        send_request(send_to, "block.getdata", body);
    }

    // Whether or not a connected peer got the getdata, also fire a
    // DHT-search for this hash so peers outside our current connect
    // set can surface and we can dial them. Rate-limited so a tight
    // catch-up doesn't hammer the DHT.
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        const auto now = std::chrono::steady_clock::now();
        auto it = dht_searched_at_.find(hash);
        if (it == dht_searched_at_.end() ||
            now - it->second >= kDhtSearchMinGap)
        {
            dht_searched_at_[hash] = now;
            dht_announce(dht_key_for_block(hash));
        }
    }
}

// =====================================================================
// stall_loop — re-issue getdata if a peer didn't respond
// =====================================================================

void BlockPropagator::stall_loop() {
    while (running_) {
        for (int i = 0; i < 5 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!running_) return;

        std::vector<Hash256> to_retry;
        {
            std::lock_guard<std::mutex> lk(peers_mu_);
            const auto now = std::chrono::steady_clock::now();
            for (auto& [pid, ps] : peers_) {
                if (ps.in_flight.empty()) continue;
                if (now - ps.in_flight_since < kGetdataStall) continue;
                for (const auto& h : ps.in_flight) to_retry.push_back(h);
                ps.in_flight.clear();
                ps.in_flight_since = {};
            }
        }
        for (const auto& h : to_retry) {
            // schedule_getdata picks a different peer (the original is
            // now empty-in_flight).
            schedule_getdata(h);
        }
    }
}

// =====================================================================
// seed_dial_loop — explicit sync_seeds fallback
// =====================================================================

void BlockPropagator::seed_dial_loop() {
    while (running_) {
        if (client_ && !cfg_.sync_seeds.empty()) {
            for (const auto& seed : cfg_.sync_seeds) {
                auto colon = seed.find_last_of(':');
                if (colon == std::string::npos) continue;
                int port = 0;
                try { port = std::stoi(seed.substr(colon + 1)); }
                catch (...) { continue; }
                if (port <= 0 || port >= 65536) continue;
                rats_connect(client_, seed.substr(0, colon).c_str(), port);
            }
        }
        for (int i = 0; i < 30 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// =====================================================================
// dht_announce_loop — initial walk + periodic re-announce
// =====================================================================

void BlockPropagator::dht_announce_loop() {
    // Phase 1: announce every block we currently store, in bursts so
    // the DHT thread isn't drowned. A brand-new peer searching for
    // genesis or for any of these hashes should find us within a few
    // DHT-find rounds.
    {
        const auto tip = chain_.tip();
        uint32_t burst = 0;
        for (uint32_t h = 0; h <= tip.height && running_; ++h) {
            auto bh = chain_.get_block_hash(h);
            if (!bh) continue;
            {
                std::lock_guard<std::mutex> lk(pending_mu_);
                if (announced_.find(*bh) != announced_.end()) continue;
                announced_.insert(*bh);
            }
            dht_announce(dht_key_for_block(*bh));
            if (++burst >= kStartupAnnounceBurst) {
                burst = 0;
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }
        last_full_announce_ = std::chrono::steady_clock::now();
    }

    // Phase 2: periodic re-announce. DHT entries expire (BitTorrent
    // BEP-5 default is 30 min); we re-announce just before that so
    // our presence stays sticky.
    while (running_) {
        for (int i = 0; i < 30 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!running_) return;

        // (#8 + #14 instability fix) bounded-growth maintenance, every ~30s
        // (BEFORE the 30-min re-announce gate below, or it would barely run):
        //  - clear any peer `known` set grown past kKnownCap — the trim this
        //    map's header comment always promised but never had; a peer
        //    re-learning a hash just gets a re-INV that is deduped;
        //  - drop dht_searched_at_ entries older than a few search gaps, which
        //    no longer suppress anything and would otherwise leave one
        //    permanent entry per distinct block hash ever fetched.
        {
            std::lock_guard<std::mutex> lk(peers_mu_);
            for (auto& [pid, ps] : peers_) {
                (void)pid;
                if (ps.known.size() > kKnownCap) ps.known.clear();
            }
        }
        {
            std::lock_guard<std::mutex> lk(pending_mu_);
            const auto cutoff = std::chrono::steady_clock::now();
            for (auto it = dht_searched_at_.begin();
                 it != dht_searched_at_.end(); ) {
                if (cutoff - it->second > kDhtSearchMinGap * 4)
                    it = dht_searched_at_.erase(it);
                else ++it;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_full_announce_ < kReannounceMin) continue;

        // Re-announce bootstrap key + every announced block hash.
        dht_announce(bootstrap_key_);
        std::vector<Hash256> hashes;
        {
            std::lock_guard<std::mutex> lk(pending_mu_);
            hashes.assign(announced_.begin(), announced_.end());
        }
        uint32_t burst = 0;
        for (const auto& h : hashes) {
            if (!running_) return;
            dht_announce(dht_key_for_block(h));
            if (++burst >= kStartupAnnounceBurst) {
                burst = 0;
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }
        last_full_announce_ = std::chrono::steady_clock::now();
    }
}

} // namespace mc
