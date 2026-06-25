#pragma once
#include <cstdint>

#include "../core/block.h"  // Address
#include "../storage/database.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace mc::net {

// RelayCreditTracker — full-node-side accumulator of relay reward owed to
// each mini-node, in INTERNAL TOKEN UNITS (1 MC = 1e8 units).
//
// (#10) Crediting is now per CORROBORATED BYTE via the triangulation in
// rats_api (RatsApi::try_corroborate): the broker brokered the stream, the
// mini reported the bytes it relayed (signed), and the player confirmed the
// bytes it received (signed); we credit min(relayed,received) bytes × the
// per-byte rate. So `increment()` is now called with the already-computed
// internal-unit amount, NOT a per-stream count of 1, and the mini-node DOES
// report its own counts (cross-checked against the player's receipt).
//
// Storage: leveldb under the `rc:` prefix (`rc:<mini_addr_hex>` → u64 units).
// Survives full-node restarts so credits accrued before a sweep aren't lost.
//
// Sweep: every kSweepIntervalMs (~5 min) the tracker calls a caller-supplied
// "mint" callback with each (mini_addr, units) pair, then clears the entry.
// The callback constructs a RelayRewardTx{count = units} (count now means
// internal units, applied directly by apply_relay_reward — no per-MC scale),
// signs it (founder for now), and pushes it to the mempool.
class RelayCreditTracker {
public:
    using MintCallback = std::function<void(const Address& mini_addr,
                                            uint64_t count)>;

    static constexpr uint32_t kSweepIntervalMs = 5 * 60 * 1000;

    explicit RelayCreditTracker(Database& db);
    ~RelayCreditTracker();

    // Add credits to a mini-node. Idempotent + thread-safe.
    void increment(const Address& mini_addr, uint64_t n = 1);

    // Read the current count for a single mini-node (mostly diagnostics).
    uint64_t get(const Address& mini_addr) const;

    // Start the background sweep thread. `mint` is called per non-zero
    // entry at every sweep interval. Calling start a second time is a
    // no-op.
    void start(MintCallback mint);
    void stop();

private:
    void loop();

    Database&                   db_;
    mutable std::mutex          mu_;
    std::map<std::string, uint64_t> credits_;  // key = hex(addr)
    MintCallback                mint_;
    std::thread                 worker_;
    std::atomic<bool>           running_{false};
};

} // namespace mc::net
