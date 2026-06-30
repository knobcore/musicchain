#include "bip39.h"
#include "bip39_wordlist_en.h"
#include "hash.h"

#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <sstream>
#include <unordered_map>

namespace mc::crypto {

namespace {

// Lazily-built lookup map word -> 11-bit index. Thread-safe under
// initialization because static-local + functional construction.
const std::unordered_map<std::string, uint16_t>& word_index_map() {
    static const auto map = []{
        std::unordered_map<std::string, uint16_t> m;
        m.reserve(2048);
        for (uint16_t i = 0; i < 2048; ++i) {
            m.emplace(std::string(kBip39EnglishWordlist[i]), i);
        }
        return m;
    }();
    return map;
}

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Pack a sequence of 11-bit values into a contiguous bit-stream of
// `bit_len` bits. Used to glue word indices back into the original
// entropy + checksum payload.
std::vector<uint8_t> pack_bits(const std::vector<uint16_t>& indices,
                               size_t bit_len) {
    const size_t byte_len = (bit_len + 7) / 8;
    std::vector<uint8_t> out(byte_len, 0);
    size_t bit_pos = 0;
    for (uint16_t v : indices) {
        for (int b = 10; b >= 0; --b, ++bit_pos) {
            if (bit_pos >= bit_len) return out;
            if ((v >> b) & 1u) {
                out[bit_pos / 8] |= static_cast<uint8_t>(0x80u >> (bit_pos % 8));
            }
        }
    }
    return out;
}

// Inverse of pack_bits: split a byte buffer into N 11-bit indices.
std::vector<uint16_t> split_bits(const std::vector<uint8_t>& buf,
                                  size_t n_indices) {
    std::vector<uint16_t> out(n_indices, 0);
    size_t bit_pos = 0;
    for (size_t i = 0; i < n_indices; ++i) {
        uint16_t v = 0;
        for (int b = 0; b < 11; ++b, ++bit_pos) {
            const size_t byte = bit_pos / 8;
            if (byte >= buf.size()) break;
            uint8_t bit = (buf[byte] >> (7 - (bit_pos % 8))) & 1u;
            v = static_cast<uint16_t>((v << 1) | bit);
        }
        out[i] = v;
    }
    return out;
}

// Append a checksum to entropy. The checksum is the first `cs_bits`
// bits of SHA-256(entropy), where `cs_bits = entropy_bits / 32`.
// Returns the combined (entropy || checksum) byte payload AND the
// total bit length (which is not always a byte multiple).
std::pair<std::vector<uint8_t>, size_t>
append_checksum(const std::vector<uint8_t>& entropy) {
    const size_t ent_bits = entropy.size() * 8;
    const size_t cs_bits  = ent_bits / 32;          // 4 for 128 bits, 8 for 256
    const size_t total    = ent_bits + cs_bits;

    auto h = sha256(entropy);
    std::vector<uint8_t> out = entropy;
    if (cs_bits <= 8) {
        // Take the top `cs_bits` of h[0].
        uint8_t mask = static_cast<uint8_t>(0xFF << (8 - cs_bits));
        out.push_back(h[0] & mask);
    } else {
        // 256-bit entropy → 8-bit checksum exactly. Append h[0] in full.
        out.push_back(h[0]);
    }
    return {out, total};
}

// Convenience: split a mnemonic into lowercased words.
std::vector<std::string> split_mnemonic(const std::string& m) {
    std::vector<std::string> out;
    std::istringstream iss(m);
    std::string w;
    while (iss >> w) out.push_back(lower_ascii(w));
    return out;
}

} // namespace

// ---- Public API -----------------------------------------------------

std::string bip39_generate_12() {
    std::array<uint8_t, 16> entropy{};
    if (RAND_bytes(entropy.data(),
                   static_cast<int>(entropy.size())) != 1) {
        return {};
    }

    auto [packed, bit_len] =
        append_checksum(std::vector<uint8_t>(entropy.begin(), entropy.end()));

    const size_t n_words = bit_len / 11;
    std::vector<uint16_t> indices = split_bits(packed, n_words);

    std::string out;
    for (size_t i = 0; i < indices.size(); ++i) {
        if (i) out.push_back(' ');
        out.append(kBip39EnglishWordlist[indices[i] & 0x7FFu]);
    }
    return out;
}

bool bip39_validate(const std::string& mnemonic) {
    auto words = split_mnemonic(mnemonic);
    const size_t n = words.size();
    if (!(n == 12 || n == 15 || n == 18 || n == 21 || n == 24)) return false;

    const auto& m = word_index_map();
    std::vector<uint16_t> indices;
    indices.reserve(n);
    for (const auto& w : words) {
        auto it = m.find(w);
        if (it == m.end()) return false;
        indices.push_back(it->second);
    }

    const size_t total_bits = n * 11;
    const size_t ent_bits   = (total_bits * 32) / 33;
    const size_t cs_bits    = total_bits - ent_bits;
    const size_t ent_bytes  = ent_bits / 8;

    auto packed = pack_bits(indices, total_bits);

    std::vector<uint8_t> entropy(packed.begin(),
                                  packed.begin() + ent_bytes);
    uint8_t got_cs = packed[ent_bytes];

    auto h = sha256(entropy);
    uint8_t mask = static_cast<uint8_t>(0xFF << (8 - cs_bits));
    uint8_t want_cs = h[0] & mask;
    got_cs &= mask;

    return want_cs == got_cs;
}

std::vector<uint8_t> bip39_mnemonic_to_seed(const std::string& mnemonic,
                                            const std::string& passphrase) {
    if (!bip39_validate(mnemonic)) return {};
    // BIP39 spec: salt = "mnemonic" + passphrase, both NFKD-normalized.
    // We accept lower-ASCII for now; full NFKD belongs in the wallet
    // layer when we support non-ASCII passphrases.
    std::string salt = "mnemonic" + passphrase;
    std::vector<uint8_t> out(64, 0);
    PKCS5_PBKDF2_HMAC(mnemonic.data(),
                      static_cast<int>(mnemonic.size()),
                      reinterpret_cast<const unsigned char*>(salt.data()),
                      static_cast<int>(salt.size()),
                      2048,
                      EVP_sha512(),
                      64,
                      out.data());
    return out;
}

std::optional<KeyPair> bip39_mnemonic_to_keypair(
    const std::string& mnemonic, const std::string& passphrase) {
    // NOTE: legacy fallback file — not linked. The CMake target list
    // pulls wally_bip39.cpp which does proper BIP32 derivation
    // (m/44'/19779'/0'/0/0 → child priv → keypair_from_priv_bytes).
    // This stub is kept only so the file still compiles after
    // keypair_from_seed was removed. If you re-enable this file you
    // also need to add BIP32 derivation here or you'll ship the
    // wrong address for every existing libwally-derived wallet.
    auto seed = bip39_mnemonic_to_seed(mnemonic, passphrase);
    if (seed.size() != 64) return std::nullopt;
    try {
        return keypair_from_priv_bytes(seed.data());
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace mc::crypto
