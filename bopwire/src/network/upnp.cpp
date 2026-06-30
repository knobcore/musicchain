/**
 * upnp.cpp — UPnP IGD port mapper using miniupnpc.
 */
#include "upnp.h"

#include <iostream>
#include <chrono>
#include <cstring>

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>

namespace mc::net {

UpnpMapper::UpnpMapper(std::vector<PortMapping> ports)
    : ports_(std::move(ports)) {}

UpnpMapper::~UpnpMapper() { stop(); }

bool UpnpMapper::start() {
    bool ok = map_all();
    running_ = true;
    thread_ = std::thread([this]{ run(); });
    return ok;
}

void UpnpMapper::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

bool UpnpMapper::map_all() {
    int error = 0;
    // Discover UPnP IGD devices (2 s timeout).
    UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr,
                                     UPNP_LOCAL_PORT_ANY, 0, 2, &error);
    if (!devlist) {
        std::cerr << "[upnp] no IGD found (error=" << error << ")\n";
        return false;
    }

    UPNPUrls urls{};
    IGDdatas  data{};
    char      lanaddr[64]{};
    char      wanaddr[64]{};

    int igd = UPNP_GetValidIGD(devlist, &urls, &data,
                                lanaddr, sizeof(lanaddr),
                                wanaddr, sizeof(wanaddr));
    freeUPNPDevlist(devlist);

    if (igd != 1) {
        std::cerr << "[upnp] no valid IGD (result=" << igd << ")\n";
        if (igd > 0) FreeUPNPUrls(&urls);
        return false;
    }

    // wanaddr is already populated by UPNP_GetValidIGD in v2.2+.
    if (wanaddr[0] != '\0') {
        external_ip_ = wanaddr;
        std::cout << "[upnp] external IP: " << wanaddr << "\n";
    }

    bool any_ok = false;
    for (const auto& pm : ports_) {
        const std::string port_str = std::to_string(pm.port);
        // Delete any stale mapping first (ignore errors).
        UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype,
                               port_str.c_str(), pm.proto.c_str(), nullptr);

        int r = UPNP_AddPortMapping(
            urls.controlURL,
            data.first.servicetype,
            port_str.c_str(),   // external port
            port_str.c_str(),   // internal port
            lanaddr,            // internal host
            pm.desc.c_str(),    // description
            pm.proto.c_str(),   // "TCP" or "UDP"
            nullptr,            // remote host (any)
            "600"               // lease duration seconds (0 = permanent on some routers)
        );

        if (r == UPNPCOMMAND_SUCCESS) {
            std::cout << "[upnp] mapped " << pm.proto << " "
                      << wanaddr << ":" << pm.port
                      << " → " << lanaddr << ":" << pm.port
                      << " (" << pm.desc << ")\n";
            any_ok = true;
        } else {
            std::cerr << "[upnp] failed to map port " << pm.port
                      << "/" << pm.proto
                      << " — " << strupnperror(r) << "\n";
        }
    }

    FreeUPNPUrls(&urls);
    return any_ok;
}

void UpnpMapper::run() {
    // Refresh mappings every 8 minutes (lease is 10 min).
    auto last = std::chrono::steady_clock::now();
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::minutes>(now - last).count() >= 8) {
            map_all();
            last = now;
        }
    }
}

} // namespace mc::net
