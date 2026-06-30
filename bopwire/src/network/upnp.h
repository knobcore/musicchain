#pragma once
/**
 * upnp.h — UPnP IGD port mapper.
 *
 * Opens TCP port mappings for all three Bopwire ports on the router:
 *   9333 — P2P gossip (node-to-node)
 *   9334 — HTTP API  (player → node)
 *   9335 — BT seeder (player → node, bootstrap file download)
 *
 * Mappings are refreshed every 10 minutes (UPnP leases expire).
 * On stop(), mappings are deleted so the router cleans up immediately.
 */
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace mc::net {

struct PortMapping {
    uint16_t    port;
    std::string proto;   // "TCP" or "UDP"
    std::string desc;
};

class UpnpMapper {
public:
    /// ports: list of {port, proto, description} to map.
    explicit UpnpMapper(std::vector<PortMapping> ports);
    ~UpnpMapper();

    /// Attempt UPnP discovery and mapping synchronously, then keep a
    /// background thread refreshing them.  Returns true if IGD was found.
    bool start();
    void stop();

    /// External (WAN) IP discovered via UPnP, empty if not found.
    std::string external_ip() const { return external_ip_; }

private:
    std::vector<PortMapping> ports_;
    std::string              external_ip_;
    std::atomic<bool>        running_{false};
    std::thread              thread_;

    bool map_all();   // returns true if at least one mapping succeeded
    void run();
};

} // namespace mc::net
