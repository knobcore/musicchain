#include "relay_credit_tracker.h"
#include "../crypto/hash.h"

#include <cstring>
#include <iostream>

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
        if (snapshot.empty()) continue;

        for (auto& [addr_hex, count] : snapshot) {
            if (count == 0) continue;
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
                if (mint_) mint_(a, count);
            } catch (...) { /* don't let a bad mint cb take down the tracker */ }
            db_.del("rc:" + addr_hex);
        }
    }
}

} // namespace mc::net
