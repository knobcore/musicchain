#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace mc::net {

// Operator-tunable knobs read from the JSON config file. All fields
// have sensible defaults; an operator only sets the ones they want to
// override. Lives in config.json under the "load_monitor" key:
//
//   "load_monitor": {
//       "max_bandwidth_bps": 12500000,      // = 100 Mbps. Score hits
//                                            // 1.0 at this RX+TX rate.
//       "cpu_weight":        0.6,           // 0..1; weight of CPU term
//       "net_weight":        0.4,           // 0..1; weight of net term
//       "sample_interval_ms": 5000,         // how often to sample
//       "busy_score_threshold": 0.85,       // emit "busy" log at >=
//       "disable_net_metric": false,        // ignore network entirely
//       "disable_cpu_metric": false         // ignore CPU entirely
//   }
struct LoadConfig {
    uint64_t max_bandwidth_bps    = 10ull * 1024 * 1024;  // ~80 Mbps
    float    cpu_weight            = 0.6f;
    float    net_weight            = 0.4f;
    uint32_t sample_interval_ms    = 5000;
    float    busy_score_threshold  = 0.85f;
    bool     disable_net_metric    = false;
    bool     disable_cpu_metric    = false;
};

// LoadMonitor samples this process's CPU + network usage on a thread and
// exposes a single normalized load score in [0.0, 1.0]. Both the full
// node and the mini-node spin one up and publish the snapshot in their
// routes/status records, so players can pick the lightest peer.
//
// Score:
//   load_score = clamp(cpu_weight * cpu_load
//                    + net_weight * (net_bps / max_bandwidth_bps), 0, 1)
//   0.0 = idle, 1.0 = saturated
//
// Logging: when load_score >= busy_score_threshold we emit a "[load]
// node busy" line so operators see backpressure in the foreground log.
//
// Snapshot fields are published as-is so consumers (player routing,
// debug RPC) can apply their own weighting.
class LoadMonitor {
public:
    struct Snapshot {
        float    cpu_load          = 0.0f;
        uint64_t net_bytes_per_sec = 0;
        float    load_score        = 0.0f;
        bool     is_busy           = false;
        uint64_t timestamp_ms      = 0;
    };

    LoadMonitor();
    explicit LoadMonitor(LoadConfig cfg);
    ~LoadMonitor();

    void start();
    void stop();

    Snapshot   current() const;
    LoadConfig config()  const;

private:
    void loop();

    LoadConfig         cfg_;
    mutable std::mutex mu_;
    Snapshot           snap_{};
    std::thread        worker_;
    std::atomic<bool>  running_{false};
};

} // namespace mc::net
