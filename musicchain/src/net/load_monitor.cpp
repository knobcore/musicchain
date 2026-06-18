#include "load_monitor.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <iphlpapi.h>
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "iphlpapi.lib")
#include <vector>
#else
#include <unistd.h>
#include <vector>
#endif

namespace mc::net {

namespace {

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// ---- Per-platform sampling helpers ----------------------------------

#ifdef _WIN32

// Windows: read this-process kernel+user time deltas vs wall clock for
// CPU; sum all-interface ulRxBytes+ulTxBytes from GetIfTable for network.
struct CpuSample {
    uint64_t process_100ns = 0;
    uint64_t wall_100ns    = 0;
};
CpuSample sample_cpu() {
    CpuSample s{};
    FILETIME creation, exit, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        ULARGE_INTEGER k, u;
        k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime;   u.HighPart = user.dwHighDateTime;
        s.process_100ns = k.QuadPart + u.QuadPart;
    }
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER wall; wall.LowPart = ft.dwLowDateTime; wall.HighPart = ft.dwHighDateTime;
    s.wall_100ns = wall.QuadPart;
    return s;
}

uint64_t sample_net_bytes() {
    // GetIfTable returns per-adapter in/out byte counts. Sum across
    // adapters; not perfect per-process but on a node-dedicated VPS or
    // home node it's a good proxy. We could read /proc/net/dev style
    // counters from Windows ETW for true per-process numbers later.
    PMIB_IFTABLE table = nullptr;
    DWORD size = 0;
    GetIfTable(table, &size, FALSE);
    if (size == 0) return 0;
    std::vector<uint8_t> buf(size);
    table = reinterpret_cast<PMIB_IFTABLE>(buf.data());
    if (GetIfTable(table, &size, FALSE) != NO_ERROR) return 0;
    uint64_t total = 0;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        total += table->table[i].dwInOctets;
        total += table->table[i].dwOutOctets;
    }
    return total;
}

#else

// Linux: /proc/self/stat for CPU jiffies (fields 14, 15 = utime, stime),
// /proc/net/dev for interface byte counts.
struct CpuSample {
    uint64_t process_ticks = 0;
    uint64_t wall_ms       = 0;
};
CpuSample sample_cpu() {
    CpuSample s{};
    std::ifstream f("/proc/self/stat");
    if (f) {
        std::string content; std::getline(f, content);
        // Field 2 is the command in parens — strip it so the field
        // counting after isn't thrown by spaces inside.
        auto rp = content.rfind(')');
        if (rp != std::string::npos && rp + 2 < content.size()) {
            std::istringstream iss(content.substr(rp + 2));
            std::string tok;
            int idx = 3; // we already consumed pid + comm
            uint64_t utime = 0, stime = 0;
            while (iss >> tok) {
                if (idx == 14) utime = std::stoull(tok);
                else if (idx == 15) { stime = std::stoull(tok); break; }
                ++idx;
            }
            s.process_ticks = utime + stime;
        }
    }
    s.wall_ms = now_ms();
    return s;
}

uint64_t sample_net_bytes() {
    std::ifstream f("/proc/net/dev");
    if (!f) return 0;
    std::string line;
    // Skip the two header lines.
    std::getline(f, line); std::getline(f, line);
    uint64_t total = 0;
    while (std::getline(f, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::istringstream iss(line.substr(colon + 1));
        uint64_t rx_bytes = 0, tx_bytes = 0, tmp = 0;
        // Format: rx_bytes rx_pkts rx_err rx_drop rx_fifo rx_frame rx_compressed rx_multicast
        //         tx_bytes tx_pkts ...
        iss >> rx_bytes;
        for (int i = 0; i < 7; ++i) iss >> tmp;
        iss >> tx_bytes;
        total += rx_bytes + tx_bytes;
    }
    return total;
}

#endif

} // namespace

LoadMonitor::LoadMonitor() : cfg_(LoadConfig{}) {}
LoadMonitor::LoadMonitor(LoadConfig cfg) : cfg_(std::move(cfg)) {}
LoadMonitor::~LoadMonitor() { stop(); }

LoadConfig LoadMonitor::config() const { return cfg_; }

void LoadMonitor::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread([this]{ loop(); });
}
void LoadMonitor::stop() {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) worker_.join();
}

LoadMonitor::Snapshot LoadMonitor::current() const {
    std::lock_guard<std::mutex> lk(mu_);
    return snap_;
}

void LoadMonitor::loop() {
    auto prev_cpu = sample_cpu();
    auto prev_net = sample_net_bytes();
    while (running_) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(cfg_.sample_interval_ms));
        if (!running_) break;
        auto cpu = sample_cpu();
        auto net = sample_net_bytes();

        float cpu_load = 0.0f;
        if (!cfg_.disable_cpu_metric) {
#ifdef _WIN32
            const uint64_t dproc = cpu.process_100ns - prev_cpu.process_100ns;
            const uint64_t dwall = cpu.wall_100ns    - prev_cpu.wall_100ns;
            if (dwall > 0) cpu_load = static_cast<float>(dproc) / dwall;
#else
            const long hz = sysconf(_SC_CLK_TCK);
            const uint64_t dticks = cpu.process_ticks - prev_cpu.process_ticks;
            const uint64_t dwall_ms = cpu.wall_ms - prev_cpu.wall_ms;
            if (hz > 0 && dwall_ms > 0) {
                const float cpu_sec  = static_cast<float>(dticks) / hz;
                const float wall_sec = dwall_ms / 1000.0f;
                cpu_load = cpu_sec / wall_sec;
            }
#endif
            cpu_load = std::clamp(cpu_load, 0.0f, 1.0f);
        }

        uint64_t bps = 0;
        if (!cfg_.disable_net_metric) {
            const uint64_t dnet = net > prev_net ? (net - prev_net) : 0;
            bps = (dnet * 1000ull) / cfg_.sample_interval_ms;
        }

        const float net_term = cfg_.disable_net_metric ? 0.0f :
            std::clamp(static_cast<float>(bps) / cfg_.max_bandwidth_bps,
                       0.0f, 1.0f);
        const float score = std::clamp(
            cfg_.cpu_weight * cpu_load + cfg_.net_weight * net_term,
            0.0f, 1.0f);

        Snapshot s;
        s.cpu_load          = cpu_load;
        s.net_bytes_per_sec = bps;
        s.load_score        = score;
        s.is_busy           = score >= cfg_.busy_score_threshold;
        s.timestamp_ms      = now_ms();
        if (s.is_busy) {
            std::cout << "[load] node busy: score=" << score
                      << " cpu=" << cpu_load
                      << " net_bps=" << bps << "\n";
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            snap_ = s;
        }
        prev_cpu = cpu;
        prev_net = net;
    }
}

} // namespace mc::net
