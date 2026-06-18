#include "keccak256_c.h"
#include <string.h>

/* Plain-C port of keccak256.cpp. Same constants, same algorithm. */

static const uint64_t kRoundConstants[24] = {
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
static const int kRho[24] = {
     1,  3,  6, 10, 15, 21, 28, 36, 45, 55,  2, 14,
    27, 41, 56,  8, 25, 43, 62, 18, 39, 61, 20, 44,
};
static const int kPi[24] = {
    10,  7, 11, 17, 18,  3,  5, 16,  8, 21, 24,  4,
    15, 23, 19, 13, 12,  2, 20, 14, 22,  9,  6,  1,
};

static uint64_t rotl64(uint64_t v, int n) {
    return (v << n) | (v >> (64 - n));
}

static void keccak_f(uint64_t s[25]) {
    int round;
    for (round = 0; round < 24; ++round) {
        uint64_t c[5];
        int x, y, i;
        for (x = 0; x < 5; ++x)
            c[x] = s[x] ^ s[x+5] ^ s[x+10] ^ s[x+15] ^ s[x+20];
        for (x = 0; x < 5; ++x) {
            uint64_t d = c[(x+4) % 5] ^ rotl64(c[(x+1) % 5], 1);
            for (y = 0; y < 25; y += 5) s[x+y] ^= d;
        }
        {
            uint64_t t = s[1];
            for (i = 0; i < 24; ++i) {
                int j = kPi[i];
                uint64_t tmp = s[j];
                s[j] = rotl64(t, kRho[i]);
                t = tmp;
            }
        }
        for (y = 0; y < 25; y += 5) {
            uint64_t b[5];
            for (x = 0; x < 5; ++x) b[x] = s[y+x];
            for (x = 0; x < 5; ++x)
                s[y+x] = b[x] ^ ((~b[(x+1) % 5]) & b[(x+2) % 5]);
        }
        s[0] ^= kRoundConstants[round];
    }
}

static uint64_t load64_le(const uint8_t* p) {
    uint64_t v = 0; int i;
    for (i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8*i);
    return v;
}
static void store64_le(uint8_t* p, uint64_t v) {
    int i;
    for (i = 0; i < 8; ++i) p[i] = (uint8_t)((v >> (8*i)) & 0xFF);
}

void mc_keccak256(const uint8_t* data, size_t len, uint8_t out[32]) {
    /* Keccak-256: rate = 136 bytes, padding byte 0x01 (Ethereum style). */
    const size_t kRate = 136;
    uint64_t state[25];
    uint8_t  block[136];
    size_t   i;
    memset(state, 0, sizeof(state));

    while (len >= kRate) {
        for (i = 0; i < kRate; i += 8)
            state[i/8] ^= load64_le(data + i);
        keccak_f(state);
        data += kRate;
        len  -= kRate;
    }

    memset(block, 0, kRate);
    memcpy(block, data, len);
    block[len]         = 0x01;
    block[kRate - 1] |= 0x80;
    for (i = 0; i < kRate; i += 8)
        state[i/8] ^= load64_le(block + i);
    keccak_f(state);

    for (i = 0; i < 32; i += 8)
        store64_le(out + i, state[i/8]);
}
