#pragma once

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

// RelayCreditTracker — full-node-side bookkeeping of how many times each
// known mini-node has served as a tunnel for our binary traffic. The
// mini-node never reports its own counts; the full node observes its
// own outbound deliveries and credits the conduit.
//
// Storage: leveldb under the `rc:` prefix (`rc:<mini_addr_hex>` → u64).
// Survives full-node restarts so credits accrued before a sweep aren't
// lost when the operator bounces the service.
//
// Sweep: every kSweepIntervalMs (~5 min), the tracker calls a caller-
// supplied "mint" callback with each (mini_addr, count) pair, then
// clears that entry. The callback constructs a RelayRewardTx, signs it
// with the founder key, and pushes it into the mempool. We can't
// know the mini-node's wallet address at config-time — we learn it
// dynamically via the routes.get response (which now includes a
// `wallet` field per the mini-node patch).
//
// Identifying "this request came in via a mini-node tunnel" lives at
// the rats_api layer; the tracker exposes a simple increment() that
// the API hook calls. The hook decides whether to count.
//
// SCOPE: only count streams + downloads, i.e. binary payload deliveries
// that justify a mini-node operator hosting the relay. Chat /
// moderation / routes RPCs are control-plane and don't qualify. The
// canonical list of credit-earning verbs (single source of truth so
// the policy isn't scattered across handlers):
//
//   - stream.open         → audio stream session start
//   - song.audio          → full content download chunk
//   - song.get            → metadata pull preceding a download
//
// Any other verb (chat.*, routes.*, mod.*) MUST NOT call increment().
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
