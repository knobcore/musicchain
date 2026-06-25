/**
 * capi.cpp - Implementation of the public C API (musicchain.h).
 * Bridges C-style API to the C++ internals for Flutter FFI use.
 */
#include "../../include/musicchain.h"
#include "../crypto/hash.h"
#include "../crypto/keys.h"
#include "../crypto/signature.h"
#include "../crypto/bip39.h"
#include "../audio/ogg_validator.h"
#include "../audio/ogg_decoder.h"
#include "../audio/fingerprint.h"
#include "../core/block.h"
#include "../util/hw_fingerprint.h"

#include <cstring>
#include <cstdlib>
#include <string>
#include <memory>

// Thread-local last error string
static thread_local std::string g_last_error;

static void set_error(const std::string& msg) { g_last_error = msg; }

static char* make_cstring(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (p) { std::memcpy(p, s.data(), s.size()); p[s.size()] = '\0'; }
    return p;
}

// ---- Init / cleanup -------------------------------------------------

int mc_init(void) {
    // Nothing required currently
    return 0;
}

void mc_cleanup(void) {}

void mc_free(void* ptr) { std::free(ptr); }

const char* mc_last_error(void) {
    return g_last_error.empty() ? nullptr : g_last_error.c_str();
}

// ---- Wallet ---------------------------------------------------------

struct WalletHandle {
    mc::crypto::KeyPair kp;
};

// ---- BIP39 mnemonic wallet flow -------------------------------------
//
// NB: the function definitions here drop the `extern "C" MUSICCHAIN_API`
// decoration. The header (`include/musicchain.h`) carries it via the
// `MUSICCHAIN_API` macro which expands to dllexport when MUSICCHAIN_BUILD
// is defined and dllimport otherwise. Re-decorating the definition
// triggers C2491 ("definition of dllimport function not allowed") in
// MSVC because we're building the static library here, not the export
// side.

char* mc_bip39_generate_12(void) {
    auto s = mc::crypto::bip39_generate_12();
    if (s.empty()) {
        set_error("bip39_generate failed");
        return nullptr;
    }
    return make_cstring(s);
}

int mc_bip39_validate(const char* mnemonic) {
    if (!mnemonic) return 0;
    return mc::crypto::bip39_validate(mnemonic) ? 1 : 0;
}

mc_wallet_t mc_wallet_from_mnemonic(
    const char* mnemonic, const char* passphrase) {
    if (!mnemonic) { set_error("mnemonic is null"); return nullptr; }
    try {
        auto kp = mc::crypto::bip39_mnemonic_to_keypair(
            mnemonic, passphrase ? passphrase : "");
        if (!kp) { set_error("mnemonic failed validation"); return nullptr; }
        return new WalletHandle{*kp};
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

void mc_wallet_free(mc_wallet_t wallet) {
    delete static_cast<WalletHandle*>(wallet);
}

char* mc_wallet_get_address(mc_wallet_t wallet) {
    auto* w = static_cast<WalletHandle*>(wallet);
    if (!w) { set_error("null wallet"); return nullptr; }
    return make_cstring(mc::crypto::to_checksum_hex(w->kp.address));
}

char* mc_wallet_get_eth_address(mc_wallet_t wallet) {
    return mc_wallet_get_address(wallet);
}

char* mc_wallet_get_public_key(mc_wallet_t wallet) {
    auto* w = static_cast<WalletHandle*>(wallet);
    return make_cstring(mc::crypto::to_hex(w->kp.public_key.data(), 33));
}

char* mc_wallet_sign(mc_wallet_t wallet, const uint8_t* data, size_t len) {
    try {
        auto* w = static_cast<WalletHandle*>(wallet);
        auto sig = mc::crypto::sign_data(data, len, w->kp.private_key);
        return make_cstring(mc::crypto::to_hex(sig.data(), 64));
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

// ---- Device fingerprint (#5 structural attestation) -----------------

char* mc_device_fingerprint(void) {
    try {
        const std::string fp = mc::util::device_fingerprint_hex();
        if (fp.empty()) {
            set_error("no hardware identifier readable");
            return nullptr;
        }
        return make_cstring(fp);
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

// ---- Audio decoding -------------------------------------------------

mc_decoder_t mc_decoder_open(const uint8_t* data, size_t len) {
    try {
        auto dec = mc::audio::OggDecoder::open(data, len);
        return dec.release();
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

void mc_decoder_free(mc_decoder_t decoder) {
    delete static_cast<mc::audio::OggDecoder*>(decoder);
}

int mc_decoder_get_sample_rate(mc_decoder_t decoder) {
    return static_cast<mc::audio::OggDecoder*>(decoder)->sample_rate();
}

int mc_decoder_get_channels(mc_decoder_t decoder) {
    return static_cast<mc::audio::OggDecoder*>(decoder)->channels();
}

uint32_t mc_decoder_get_duration_ms(mc_decoder_t decoder) {
    return static_cast<mc::audio::OggDecoder*>(decoder)->duration_ms();
}

int mc_decoder_read(mc_decoder_t decoder, int16_t* buf, int max_samples) {
    return static_cast<mc::audio::OggDecoder*>(decoder)->read(buf, max_samples);
}

int mc_decoder_seek(mc_decoder_t decoder, uint32_t position_ms) {
    return static_cast<mc::audio::OggDecoder*>(decoder)->seek(position_ms) ? 0 : -1;
}

uint32_t mc_decoder_position_ms(mc_decoder_t decoder) {
    return static_cast<mc::audio::OggDecoder*>(decoder)->position_ms();
}

// ---- Checksum -------------------------------------------------------

uint32_t mc_compute_checksum(const int16_t* samples, int count) {
    uint64_t acc = 0;
    for (int i = 0; i < count; ++i) {
        int16_t s = samples[i];
        acc += static_cast<uint64_t>(s < 0 ? -s : s);
    }
    return static_cast<uint32_t>(acc & 0xFFFFFFFFULL);
}

// ---- Fingerprinting -------------------------------------------------

mc_fingerprint_t mc_fingerprint_generate(const uint8_t* data, size_t len) {
    try {
        auto fp = mc::audio::Fingerprint::from_ogg(data, len);
        return fp.release();
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

mc_fingerprint_t mc_fingerprint_from_compressed(const char* base64) {
    try {
        auto fp = mc::audio::Fingerprint::from_compressed(base64);
        if (!fp) { set_error("decompress failed"); return nullptr; }
        return fp.release();
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

void mc_fingerprint_free(mc_fingerprint_t fp) {
    delete static_cast<mc::audio::Fingerprint*>(fp);
}

char* mc_fingerprint_get_compressed(mc_fingerprint_t fp) {
    auto compressed = static_cast<mc::audio::Fingerprint*>(fp)->compressed();
    return make_cstring(compressed);
}

float mc_fingerprint_compare(mc_fingerprint_t fp_a, mc_fingerprint_t fp_b) {
    auto* a = static_cast<mc::audio::Fingerprint*>(fp_a);
    auto* b = static_cast<mc::audio::Fingerprint*>(fp_b);
    return a->similarity(*b);
}

// ---- Block parsing --------------------------------------------------

int64_t mc_block_find_separator(const uint8_t* data, size_t len) {
    if (len < mc::SEPARATOR_LENGTH) return -1;
    for (size_t i = 0; i <= len - mc::SEPARATOR_LENGTH; ++i) {
        bool found = true;
        for (size_t j = 0; j < mc::SEPARATOR_LENGTH; ++j) {
            if (data[i + j] != mc::SEPARATOR_BYTE) { found = false; break; }
        }
        if (found) return static_cast<int64_t>(i);
    }
    return -1;
}

int mc_block_extract_audio(const uint8_t* /*block_data*/, size_t /*block_len*/,
                            uint8_t** /*ogg_out*/, size_t* /*ogg_len_out*/) {
    // Block v2 no longer carries audio bytes; they live in the home
    // node's content-addressed audio store. Callers must fetch by
    // content_hash via verb_song_audio / stream.open instead. The export
    // is retained so the existing C API does not break ABI; it just
    // returns "unsupported" until something rewires it.
    set_error("mc_block_extract_audio: removed in block format v2 — "
              "fetch by content_hash from the audio store");
    return -1;
}

// ---- Utilities ------------------------------------------------------

void mc_sha256(const uint8_t* data, size_t len, uint8_t* out_hash) {
    auto h = mc::crypto::sha256(data, len);
    std::memcpy(out_hash, h.data(), 32);
}

char* mc_bytes_to_hex(const uint8_t* data, size_t len) {
    return make_cstring(mc::crypto::to_hex(data, len));
}

int mc_hex_to_bytes(const char* hex, uint8_t** out) {
    auto bytes = mc::crypto::from_hex(hex);
    if (bytes.empty()) return -1;
    *out = static_cast<uint8_t*>(std::malloc(bytes.size()));
    if (*out) std::memcpy(*out, bytes.data(), bytes.size());
    return static_cast<int>(bytes.size());
}

// ---- Audio format detection and validation --------------------------

int mc_detect_format(const uint8_t* data, size_t len) {
    if (mc::audio::is_ogg_magic(data, len)) return MC_FORMAT_OGG;
    if (mc::audio::is_mp3_magic(data, len)) return MC_FORMAT_MP3;
    return MC_FORMAT_UNKNOWN;
}

int mc_validate_audio(const uint8_t* data, size_t len, char** error_out) {
    auto result = mc::audio::validate_audio(data, len);
    if (!result.valid) {
        if (error_out) *error_out = make_cstring(result.error);
        return 0;
    }
    return 1;
}

uint32_t mc_audio_duration_ms(const uint8_t* data, size_t len) {
    auto result = mc::audio::validate_audio(data, len);
    return result.valid ? result.info.duration_ms : 0;
}
