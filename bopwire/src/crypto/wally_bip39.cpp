// libwally-core-backed implementation of the public BIP39 + BIP32 API
// declared in bip39.h. The old hand-rolled implementation
// (bip39.cpp + bip39_wordlist_en.cpp) stays in the source tree as a
// reference / fallback but is no longer linked — the CMakeLists target
// list pulls THIS file instead.
//
// Why the swap matters: libwally is the same audit-pedigree primitives
// that Blockstream / Liquid / many Bitcoin wallets use. Exchanges
// integrating bopwire already trust it, so wallet derivation
// becomes a one-day integration for them instead of a one-quarter
// review of our hand-rolled code.
//
// API stays bit-identical: the C-API exports in capi.cpp call into
// these functions and don't know or care which backend produces the
// bytes.

#include "bip39.h"
#include "hash.h"
#include <wally_core.h>
#include <wally_bip39.h>
#include <wally_bip32.h>
#include <openssl/rand.h>  // RAND_bytes — wally doesn't ship random-byte generation,
                            // only entropy consumption. OpenSSL is already
                            // linked everywhere wally_bip39.cpp is.

#include <cstring>
#include <memory>
#include <vector>

namespace mc::crypto {

namespace {

struct WallyInit {
    WallyInit() { wally_init(0); }
    ~WallyInit() { wally_cleanup(0); }
};
static WallyInit g_wally_init;

// RAII wrapper around the heap-allocated mnemonic libwally returns —
// wally_string_free is required, plain free() segfaults on Windows
// because wally allocates inside its own DLL heap.
struct WallyStr {
    char* p = nullptr;
    ~WallyStr() { if (p) wally_free_string(p); }
};

} // namespace

std::string bip39_generate_12() {
    // 128 bits of entropy → 12 words. We fill entropy with bytes from the
    // OS RNG via libwally's wally_get_random_bytes, NOT
    // wally_secp_randomize (which CONSUMES caller-supplied entropy to
    // re-randomize the internal secp256k1 context rather than producing
    // any output). Calling wally_secp_randomize on a zero buffer used to
    // succeed but leave the buffer all zeros — every "fresh" mnemonic
    // came out the same, which is what the user saw as "bootstrap:
    // entropy source failed" when the chain rejected the resulting
    // founder key.
    unsigned char entropy[16] = {};
    if (RAND_bytes(entropy, sizeof(entropy)) != 1) {
        return {};
    }
    // Fold the fresh entropy into the secp256k1 context too — defense in
    // depth against side-channel reads of the context internals.
    (void)wally_secp_randomize(entropy, sizeof(entropy));

    struct words* word_list = nullptr;
    if (bip39_get_wordlist(nullptr /* default English */, &word_list) != WALLY_OK) {
        return {};
    }
    WallyStr out;
    if (bip39_mnemonic_from_bytes(word_list, entropy, sizeof(entropy), &out.p) != WALLY_OK) {
        return {};
    }
    return std::string(out.p ? out.p : "");
}

bool bip39_validate(const std::string& mnemonic) {
    if (mnemonic.empty()) return false;
    struct words* word_list = nullptr;
    if (bip39_get_wordlist(nullptr, &word_list) != WALLY_OK) return false;
    return ::bip39_mnemonic_validate(word_list, mnemonic.c_str()) == WALLY_OK;
}

std::vector<uint8_t> bip39_mnemonic_to_seed(const std::string& mnemonic,
                                            const std::string& passphrase) {
    if (!bip39_validate(mnemonic)) return {};
    std::vector<uint8_t> seed(BIP39_SEED_LEN_512, 0);
    size_t written = 0;
    if (::bip39_mnemonic_to_seed(mnemonic.c_str(),
                                 passphrase.c_str(),
                                 seed.data(),
                                 seed.size(),
                                 &written) != WALLY_OK) {
        return {};
    }
    seed.resize(written);
    return seed;
}

std::optional<KeyPair> bip39_mnemonic_to_keypair(
    const std::string& mnemonic, const std::string& passphrase) {
    auto seed = bip39_mnemonic_to_seed(mnemonic, passphrase);
    if (seed.size() != BIP39_SEED_LEN_512) return std::nullopt;

    // BIP32 master extended key from the 64-byte BIP39 seed, then
    // derive the standard m/44'/coin'/0'/0/0 path so two devices with
    // the same mnemonic produce the same first-account address —
    // exchanges expect this.
    //
    // We use SLIP-44 coin type 19779 (= MC_CHAIN_ID, "MC") which we'll
    // register once stable. Until then any derivation path is internal;
    // sticking to BIP44 keeps the door open for hardware wallets.
    constexpr uint32_t kPurpose  = 44u  | BIP32_INITIAL_HARDENED_CHILD;
    constexpr uint32_t kCoinType = 19779u | BIP32_INITIAL_HARDENED_CHILD;
    constexpr uint32_t kAccount  = 0u   | BIP32_INITIAL_HARDENED_CHILD;
    constexpr uint32_t kChange   = 0u;
    constexpr uint32_t kIndex    = 0u;

    struct ext_key master_key{};
    if (bip32_key_from_seed(seed.data(), seed.size(),
                            BIP32_VER_MAIN_PRIVATE, 0,
                            &master_key) != WALLY_OK) {
        return std::nullopt;
    }

    uint32_t path[] = { kPurpose, kCoinType, kAccount, kChange, kIndex };
    struct ext_key child{};
    if (bip32_key_from_parent_path(&master_key,
                                   path, sizeof(path) / sizeof(path[0]),
                                   BIP32_FLAG_KEY_PRIVATE,
                                   &child) != WALLY_OK) {
        return std::nullopt;
    }

    // child.priv_key[0] is the secp256k1 prefix byte (0x00 for
    // BIP32-extended), priv_key[1..33] is the actual 32-byte scalar.
    // Use keypair_from_priv_bytes — NOT keypair_from_seed — because the
    // BIP32 child key is already the secp256k1 private key. Hashing it
    // again would produce a different address from every other EVM
    // wallet (MetaMask, ethers.js, the Android NDK
    // mc_wallet_from_mnemonic) for the same mnemonic and path.
    try {
        return keypair_from_priv_bytes(child.priv_key + 1);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace mc::crypto
