#include "relay_credit_tracker.h"
#include "../crypto/hash.h"

#include <nlohmann/json.hpp>   // pd: row parse for the TTL prune (#10)
#include <cstring>
#include <iostream>
#include <vector>

namespace mc::net {

namespace {
std::string addr_hex_key(const Address& addr) {
    return crypto::to_hex(addr.data(), addr.size());
}
uint64_t u64_from_bytes(const std::vector<uint8_t>& v) {
    if (v.size() < 8) return 0;
    uint64_t out = 0;
    for (int i = 0; i < 8; ++i) out |= static_cast<uint64_t>(v[i]) << (8 * i);
    return out;
}
std::vector<uint8_t> u64_to_bytes(uint64_t v) {
    std::vector<uint8_t> out(8);
    for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xff);
    return out;
}
} // namespace

RelayCreditTracker::RelayCreditTracker(Database& db) : db_(db) {
    // Hydrate the in-memory map from disk so credits accrued before the
    // last clean shutdown still get swept on the next sweep cycle.
    db_.for_each_with_prefix("rc:",
        [this](const std::string& key, const std::string& value) {
            if (key.size() <= 3) return true; // skip the bare prefix
            std::string addr_hex = key.substr(3);
            std::vector<uint8_t> raw(value.begin(), value.end());
            credits_[addr_hex] = u64_from_bytes(raw);
            return true;
        });
}

RelayCreditTracker::~RelayCreditTracker() { stop(); }

void RelayCreditTracker::increment(const Address& mini_addr, uint64_t n) {
    if (n == 0) return;
    const std::string key = addr_hex_key(mini_addr);
    std::lock_guard<std::mutex> lk(mu_);
    auto& c = credits_[key];
    c += n;
    // Persist immediately — small write, infrequent enough that the
    // overhead doesn't matter, and crash-safe.
    db_.put("rc:" + key, u64_to_bytes(c));
}

uint64_t RelayCreditTracker::get(const Address& mini_addr) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = credits_.find(addr_hex_key(mini_addr));
    return it == credits_.end() ? 0 : it->second;
}

void RelayCreditTracker::start(MintCallback mint) {
    if (running_.exchange(true)) return;
    mint_ = std::move(mint);
    worker_ = std::thread([this] { loop(); });
}

void RelayCreditTracker::stop() {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) worker_.join();
}

void RelayCreditTracker::loop() {
    while (running_) {
        // Sleep in short increments so stop() unblocks promptly.
        for (uint32_t elapsed = 0;
             running_ && elapsed < kSweepIntervalMs;
             elapsed += 1000) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!running_) return;

        // Snapshot under the lock, then call mint_ outside it so the
        // callback can do heavyweight work (sign, queue, etc.) without
        // holding back increment() callers.
        std::map<std::string, uint64_t> snapshot;
        {
            std::lock_guard<std::mutex> lk(mu_);
            snapshot.swap(credits_);
        }

        // (#10) Prune stale pending-delivery rows (pd:) — brokered deliveries
        // that never fully corroborated within the TTL (player left / mini
        // crashed). Runs every sweep regardless of credits, bounding the table.
        {
            const uint64_t now = (uint64_t)std::chrono::duration_cast<
                std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            constexpr uint64_t kPdTtlMs = 10 * 60 * 1000;
            std::vector<std::string> stale;
            db_.for_each_with_prefix("pd:",
                [&](const std::string& k, const std::string& v){
                    auto row = nlohmann::json::parse(v, nullptr, false);
                    uint64_t created = row.is_object()
                        ? row.value("created", (uint64_t)0) : 0;
                    if (created == 0 || (now > created && now - created > kPdTtlMs))
                        stale.push_back(k);
                    return true;
                });
            for (const auto& k : stale) db_.del(k);
            if (!stale.empty())
                std::cout << "[relay] pruned " << stale.size()
                          << " stale pending-delivery rows\n";
        }

        if (snapshot.empty()) continue;

        // Chain-side cap (mirrors apply_relay_reward): a RelayRewardTx whose
        // count (now INTERNAL UNITS, #10) exceeds this is rejected. Sized to
        // cover a heavy 5-min sweep (~333 GB @ 10 units/byte = 1e12 units =
        // 10,000 MC) with room to spare. Pre-clamp here so a runaway/malicious
        // accumulator never emits an unmineable tx; the remainder carries
        // forward to the next sweep.
        constexpr uint64_t kMaxCountPerTx = 1'000'000'000'000ull;
        for (auto& [addr_hex, count] : snapshot) {
            if (count == 0) continue;
            uint64_t to_mint   = count;
            uint64_t carryover = 0;
            if (to_mint > kMaxCountPerTx) {
                carryover = to_mint - kMaxCountPerTx;
                to_mint   = kMaxCountPerTx;
                std::cerr << "[relay] units " << count
                          << " > cap " << kMaxCountPerTx
                          << " for mini " << addr_hex.substr(0, 12)
                          << "… — minting cap, carrying over "
                          << carryover << "\n";
            }
            Address a{};
            // Parse hex back to 20 bytes.
            for (size_t i = 0; i < 20 && i * 2 + 1 < addr_hex.size(); ++i) {
                auto nibble = [&](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return 0;
                };
                a[i] = static_cast<uint8_t>(
                    (nibble(addr_hex[i*2]) << 4) | nibble(addr_hex[i*2+1]));
            }
            try {
                if (mint_) mint_(a, to_mint);
            } catch (...) { /* don't let a bad mint cb take down the tracker */ }
            if (carryover > 0) {
                // Re-credit the overflow into the live map + leveldb so
                // it shows up in the next sweep instead of vanishing.
                std::lock_guard<std::mutex> lk(mu_);
                auto& c = credits_[addr_hex];
                c += carryover;
                db_.put("rc:" + addr_hex, u64_to_bytes(c));
            } else {
                db_.del("rc:" + addr_hex);
            }
        }
    }
}

} // namespace mc::net
