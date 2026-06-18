#include "hash.h"
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace mc::crypto {

Hash256 sha256(const uint8_t* data, size_t len) {
    Hash256 out{};
    SHA256(data, len, out.data());
    return out;
}

Hash256 sha256(const std::vector<uint8_t>& data) {
    return sha256(data.data(), data.size());
}

Hash256 sha256(const std::string& data) {
    return sha256(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

Hash256 sha256d(const uint8_t* data, size_t len) {
    auto first = sha256(data, len);
    return sha256(first.data(), 32);
}

std::string to_hex(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i)
        oss << std::setw(2) << static_cast<unsigned>(data[i]);
    return oss.str();
}

std::string to_hex(const Hash256& h) {
    return to_hex(h.data(), 32);
}

std::string to_hex(const std::vector<uint8_t>& v) {
    return to_hex(v.data(), v.size());
}

std::vector<uint8_t> from_hex(const std::string& hex) {
    if (hex.size() % 2 != 0) return {};
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        char hi = hex[i], lo = hex[i+1];
        auto h = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hv = h(hi), lv = h(lo);
        if (hv < 0 || lv < 0) return {};
        out.push_back(static_cast<uint8_t>((hv << 4) | lv));
    }
    return out;
}

bool parse_hash256(const std::string& hex, Hash256& out) {
    auto b = from_hex(hex);
    if (b.size() != 32) return false;
    std::copy(b.begin(), b.end(), out.begin());
    return true;
}

bool parse_address(const std::string& hex, Address& out) {
    std::string h = hex;
    // Accept 0x-prefixed input transparently (post-CEX-migration the
    // user-facing format is EIP-55 0x-checksummed, but lots of
    // internal call sites still pass bare hex).
    if (h.size() == 42 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) {
        h = h.substr(2);
    }
    auto b = from_hex(h);
    if (b.size() != 20) return false;
    std::copy(b.begin(), b.end(), out.begin());
    return true;
}

Address escrow_address_for(const Address& artist) {
    // sha256("escrow:" + artist) — first 20 bytes are the escrow Address.
    // No secp256k1 keypair maps onto this address, so the moderator-only
    // transfer endpoint is the only way to drain it; the artist cannot
    // sign a self-transfer FROM the escrow account.
    std::vector<uint8_t> seed;
    static const char kTag[] = "escrow:";
    seed.insert(seed.end(), kTag, kTag + sizeof(kTag) - 1);
    seed.insert(seed.end(), artist.begin(), artist.end());
    Hash256 h = sha256(seed.data(), seed.size());
    Address out{};
    std::copy(h.begin(), h.begin() + 20, out.begin());
    return out;
}

} // namespace mc::crypto
