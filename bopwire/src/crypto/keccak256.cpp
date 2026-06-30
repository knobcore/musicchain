#include "keccak256.h"
#include <cstring>

namespace mc::crypto {

namespace {

// ---- Keccak-f[1600] permutation primitives --------------------------

constexpr uint64_t kRoundConstants[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL,
    0x800000000000808aULL, 0x8000000080008000ULL,
    0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008aULL, 0x0000000000000088ULL,
    0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL,
    0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL,
    0x0000000080000001ULL, 0x8000000080008008ULL,
};

constexpr int kRho[24] = {
     1,  3,  6, 10, 15, 21, 28, 36, 45, 55,  2, 14,
    27, 41, 56,  8, 25, 43, 62, 18, 39, 61, 20, 44,
};

constexpr int kPi[24] = {
    10,  7, 11, 17, 18,  3,  5, 16,  8, 21, 24,  4,
    15, 23, 19, 13, 12,  2, 20, 14, 22,  9,  6,  1,
};

inline uint64_t rotl64(uint64_t v, int n) {
    return (v << n) | (v >> (64 - n));
}

// 24-round Keccak-f[1600] on a 25-lane (5×5×64-bit) state.
void keccak_f(uint64_t s[25]) {
    for (int round = 0; round < 24; ++round) {
        // θ (theta)
        uint64_t c[5];
        for (int x = 0; x < 5; ++x) {
            c[x] = s[x] ^ s[x + 5] ^ s[x + 10] ^ s[x + 15] ^ s[x + 20];
        }
        for (int x = 0; x < 5; ++x) {
            uint64_t d = c[(x + 4) % 5] ^ rotl64(c[(x + 1) % 5], 1);
            for (int y = 0; y < 25; y += 5) s[x + y] ^= d;
        }

        // ρ (rho) + π (pi)
        uint64_t t = s[1];
        for (int i = 0; i < 24; ++i) {
            int j = kPi[i];
            uint64_t tmp = s[j];
            s[j] = rotl64(t, kRho[i]);
            t = tmp;
        }

        // χ (chi)
        for (int y = 0; y < 25; y += 5) {
            uint64_t b[5];
            for (int x = 0; x < 5; ++x) b[x] = s[y + x];
            for (int x = 0; x < 5; ++x) {
                s[y + x] = b[x] ^ ((~b[(x + 1) % 5]) & b[(x + 2) % 5]);
            }
        }

        // ι (iota)
        s[0] ^= kRoundConstants[round];
    }
}

inline uint64_t load64_le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

inline void store64_le(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

} // namespace

std::array<uint8_t, 32> keccak256(const uint8_t* data, size_t len) {
    // Keccak-256 = Keccak[1600] with rate r = 1088 bits = 136 bytes,
    // capacity c = 512 bits, padding byte 0x01 (Ethereum-style — NOT
    // 0x06 like NIST SHA-3).
    constexpr size_t kRate = 136;

    uint64_t state[25] = {};

    // Absorb
    while (len >= kRate) {
        for (size_t i = 0; i < kRate; i += 8) {
            state[i / 8] ^= load64_le(data + i);
        }
        keccak_f(state);
        data += kRate;
        len  -= kRate;
    }

    // Pad: append 0x01 || 0x00... || 0x80 to fill the rate block.
    uint8_t block[kRate] = {};
    std::memcpy(block, data, len);
    block[len]       = 0x01;
    block[kRate - 1] |= 0x80;
    for (size_t i = 0; i < kRate; i += 8) {
        state[i / 8] ^= load64_le(block + i);
    }
    keccak_f(state);

    // Squeeze the first 32 bytes.
    std::array<uint8_t, 32> out{};
    for (size_t i = 0; i < 32; i += 8) {
        store64_le(out.data() + i, state[i / 8]);
    }
    return out;
}

} // namespace mc::crypto
