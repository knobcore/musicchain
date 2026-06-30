#include "hw_fingerprint.h"
#include "../crypto/hash.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <vector>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <iphlpapi.h>
#elif defined(__APPLE__)
#  include <unistd.h>
#  include <sys/utsname.h>
#  include <net/if_dl.h>
#  include <ifaddrs.h>
#else // Linux / other POSIX
#  include <unistd.h>
#  include <sys/utsname.h>
#  include <cstdio>
#  include <dirent.h>
#  include <fstream>
#endif

namespace mc::util {
namespace {

// Append "tag=value\n" to material iff value is non-empty.
void add(std::string& material, const char* tag, const std::string& value) {
    if (value.empty()) return;
    material += tag;
    material += '=';
    material += value;
    material += '\n';
}

#if defined(_WIN32)

std::string reg_string(HKEY root, const char* subkey, const char* name) {
    char buf[512];
    DWORD len = sizeof(buf);
    LSTATUS st = RegGetValueA(root, subkey, name,
                              RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                              nullptr, buf, &len);
    if (st != ERROR_SUCCESS) {
        len = sizeof(buf);
        st = RegGetValueA(root, subkey, name, RRF_RT_REG_SZ,
                          nullptr, buf, &len);
    }
    if (st != ERROR_SUCCESS || len == 0) return {};
    return std::string(buf, (len > 0 && buf[len - 1] == '\0') ? len - 1 : len);
}

std::string primary_mac() {
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                  GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 15000;
    std::vector<uint8_t> buf(size);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG r = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addrs, &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        r = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addrs, &size);
    }
    if (r != NO_ERROR) return {};
    // Collect 6-byte MACs from physical (ethernet / wifi) adapters, then
    // take the lexicographically smallest so the choice is order-stable
    // even as virtual adapters come and go.
    std::set<std::string> macs;
    static const char* hx = "0123456789abcdef";
    for (auto* a = addrs; a; a = a->Next) {
        if (a->PhysicalAddressLength != 6) continue;
        if (a->IfType != IF_TYPE_ETHERNET_CSMACD &&
            a->IfType != IF_TYPE_IEEE80211) continue;
        std::string m;
        bool all_zero = true;
        for (int i = 0; i < 6; ++i) {
            uint8_t b = a->PhysicalAddress[i];
            if (b) all_zero = false;
            m.push_back(hx[b >> 4]);
            m.push_back(hx[b & 0xF]);
        }
        if (!all_zero) macs.insert(m);
    }
    return macs.empty() ? std::string() : *macs.begin();
}

std::string collect() {
    std::string m;
    add(m, "mac", primary_mac());
    add(m, "machineguid",
        reg_string(HKEY_LOCAL_MACHINE,
                   "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid"));
    add(m, "os_product",
        reg_string(HKEY_LOCAL_MACHINE,
                   "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                   "ProductName"));
    add(m, "os_build",
        reg_string(HKEY_LOCAL_MACHINE,
                   "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                   "CurrentBuildNumber"));
    add(m, "cpu",
        reg_string(HKEY_LOCAL_MACHINE,
                   "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                   "ProcessorNameString"));
    char host[256];
    DWORD hlen = sizeof(host);
    if (GetComputerNameA(host, &hlen)) add(m, "host", std::string(host, hlen));
    return m;
}

#elif defined(__APPLE__)

std::string primary_mac() {
    struct ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) != 0 || !ifap) return {};
    std::set<std::string> macs;
    static const char* hx = "0123456789abcdef";
    for (auto* ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_LINK) continue;
        auto* sdl = reinterpret_cast<struct sockaddr_dl*>(ifa->ifa_addr);
        if (sdl->sdl_alen != 6) continue;
        const uint8_t* p =
            reinterpret_cast<const uint8_t*>(LLADDR(sdl));
        std::string s; bool all_zero = true;
        for (int i = 0; i < 6; ++i) {
            if (p[i]) all_zero = false;
            s.push_back(hx[p[i] >> 4]); s.push_back(hx[p[i] & 0xF]);
        }
        if (!all_zero) macs.insert(s);
    }
    freeifaddrs(ifap);
    return macs.empty() ? std::string() : *macs.begin();
}

std::string collect() {
    // macOS is a secondary desktop target; keep this dependency-light (MAC +
    // OS + hostname). A stronger IOPlatformUUID would need IOKit linkage; the
    // structural tier doesn't require it, and a real verifier (DeviceCheck)
    // would supersede this entirely.
    std::string m;
    add(m, "mac", primary_mac());
    struct utsname u {};
    if (uname(&u) == 0) {
        add(m, "os", std::string(u.sysname) + " " + u.release);
        add(m, "host", u.nodename);
    }
    return m;
}

#else // Linux / generic POSIX

std::string read_first_line(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::string line;
    std::getline(f, line);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' ||
                             line.back() == ' '))
        line.pop_back();
    return line;
}

std::string primary_mac() {
    // Smallest MAC across /sys/class/net/*/address, skipping loopback and
    // the all-zero placeholder. Order-stable like the Windows path.
    DIR* d = opendir("/sys/class/net");
    if (!d) return {};
    std::set<std::string> macs;
    while (struct dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == ".." || name == "lo") continue;
        std::string mac = read_first_line(
            std::string("/sys/class/net/") + name + "/address");
        // Normalize "aa:bb:..." → "aabb..." lowercase.
        std::string norm; norm.reserve(12);
        for (char c : mac) if (c != ':') norm.push_back((char)tolower((unsigned char)c));
        if (norm.size() == 12 && norm != "000000000000") macs.insert(norm);
    }
    closedir(d);
    return macs.empty() ? std::string() : *macs.begin();
}

std::string collect() {
    std::string m;
    add(m, "mac", primary_mac());
    add(m, "machine_id", read_first_line("/etc/machine-id"));
    // product_uuid is often root-only (returns empty for an unprivileged
    // app) — that's fine, it just doesn't contribute.
    add(m, "product_uuid", read_first_line("/sys/class/dmi/id/product_uuid"));
    add(m, "board_serial", read_first_line("/sys/class/dmi/id/board_serial"));
    struct utsname u {};
    if (uname(&u) == 0) {
        add(m, "os", std::string(u.sysname) + " " + u.release);
        add(m, "host", u.nodename);
    }
    return m;
}

#endif

} // namespace

std::string device_fingerprint_hex() {
    const std::string material = collect();
    if (material.empty()) return {};
    Hash256 h = crypto::sha256(
        reinterpret_cast<const uint8_t*>(material.data()), material.size());
    return crypto::to_hex(h);
}

} // namespace mc::util
