#pragma once
//
// Process-global per-verb RPC counter. Owned by rats_api.cpp (or any
// other surface that wants to publish a verb count) and consumed by
// the full node TUI's F2 page. Header-only to keep the include graph
// boring — both producers and consumers just include this.
//

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <algorithm>

namespace mc::util {

class TrafficCounters {
public:
    void bump(const std::string& verb) {
        if (verb.empty()) return;
        std::lock_guard<std::mutex> lk(mu_);
        counts_[verb].fetch_add(1, std::memory_order_relaxed);
    }

    std::vector<std::pair<std::string, uint64_t>> snapshot() const {
        std::vector<std::pair<std::string, uint64_t>> out;
        std::lock_guard<std::mutex> lk(mu_);
        out.reserve(counts_.size());
        for (const auto& [k, v] : counts_) {
            out.emplace_back(k, v.load(std::memory_order_relaxed));
        }
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b){ return a.second > b.second; });
        return out;
    }

private:
    mutable std::mutex                                    mu_;
    std::unordered_map<std::string, std::atomic<uint64_t>> counts_;
};

inline TrafficCounters& traffic_counters() {
    static TrafficCounters instance;
    return instance;
}

} // namespace mc::util
