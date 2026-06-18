#pragma once

/**
 * musicchain.h - Public C API for the MusicChain native library.
 *
 * This header is used by the Flutter player via Dart FFI.
 * All functions use C linkage to avoid name mangling.
 *
 * Memory ownership:
 *   - Strings returned by the library are allocated by mc_alloc and must be
 *     freed with mc_free().
 *   - Handles returned by open/create functions must be released with the
 *     corresponding free function.
 *   - Input buffers are not retained after the call returns.
 */

#include <stdint.h>
#include <stddef.h>

/* Export/import macro for shared library */
#ifdef _WIN32
#  ifdef BUILDING_MUSICCHAIN_DLL
#    define MUSICCHAIN_API __declspec(dllexport)
#  else
#    define MUSICCHAIN_API __declspec(dllimport)
#  endif
#else
#  define MUSICCHAIN_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- Initialization -------------------------------------------------

/** Initialize the library. Call once before any other function.
 *  Returns 0 on success, non-zero on failure. */
MUSICCHAIN_API int mc_init(void);

/** Release all library resources. */
MUSICCHAIN_API void mc_cleanup(void);

/** Free a string or buffer allocated by the library. */
MUSICCHAIN_API void mc_free(void* ptr);

// ---- Error handling -------------------------------------------------

/** Returns the last error message as a null-terminated string, or NULL. */
MUSICCHAIN_API const char* mc_last_error(void);

// ---- Wallet ---------------------------------------------------------

typedef void* mc_wallet_t;

/** Load an existing wallet from path.
 *  Returns a wallet handle, or NULL on failure. */
MUSICCHAIN_API mc_wallet_t mc_wallet_load(const char* path, const char* password);

/** Generate a fresh 12-word BIP39 English mnemonic. Caller owns the
 *  returned string and must free it with mc_free. Returns NULL on
 *  entropy-source failure (see mc_last_error). */
MUSICCHAIN_API char*        mc_bip39_generate_12(void);

/** Validate a BIP39 mnemonic against the English wordlist + checksum.
 *  Accepts 12/15/18/21/24-word phrases. Returns 1 if valid, 0 if not. */
MUSICCHAIN_API int          mc_bip39_validate(const char* mnemonic);

/** Derive a wallet from a BIP39 mnemonic + optional passphrase
 *  (passphrase may be NULL for empty). Returns NULL on failure (see
 *  mc_last_error). */
MUSICCHAIN_API mc_wallet_t  mc_wallet_from_mnemonic(const char* mnemonic,
                                                    const char* passphrase);

/** Compute the Ethereum-style Base address (0x-prefixed 40-hex,
 *  lowercase) for the wallet's secp256k1 key. Same key, different
 *  derivation — used by the Base bridge so musicchain and Base
 *  addresses are unified under one mnemonic. Caller frees with
 *  mc_free. */
MUSICCHAIN_API char*        mc_wallet_get_eth_address(mc_wallet_t wallet);

/** Save wallet to path. Returns 0 on success. */
MUSICCHAIN_API int mc_wallet_save(mc_wallet_t wallet, const char* path);

/** Release wallet handle. */
MUSICCHAIN_API void mc_wallet_free(mc_wallet_t wallet);

/** Get wallet address as a 40-character hex string (20 bytes).
 *  Caller must free with mc_free(). */
MUSICCHAIN_API char* mc_wallet_get_address(mc_wallet_t wallet);

/** Get compressed public key as a 66-character hex string (33 bytes).
 *  Caller must free with mc_free(). */
MUSICCHAIN_API char* mc_wallet_get_public_key(mc_wallet_t wallet);

/** Sign data with the wallet private key.
 *  Returns 128-character hex string (64-byte signature), or NULL on failure.
 *  Caller must free with mc_free(). */
MUSICCHAIN_API char* mc_wallet_sign(mc_wallet_t wallet, const uint8_t* data, size_t len);

// ---- Audio decoding -------------------------------------------------

typedef void* mc_decoder_t;

/** Open an Ogg/Vorbis or Ogg/Opus decoder from raw data.
 *  Returns a decoder handle, or NULL on failure. */
MUSICCHAIN_API mc_decoder_t mc_decoder_open(const uint8_t* data, size_t len);

/** Release decoder handle. */
MUSICCHAIN_API void mc_decoder_free(mc_decoder_t decoder);

/** Get sample rate in Hz. */
MUSICCHAIN_API int mc_decoder_get_sample_rate(mc_decoder_t decoder);

/** Get channel count. */
MUSICCHAIN_API int mc_decoder_get_channels(mc_decoder_t decoder);

/** Get total duration in milliseconds. */
MUSICCHAIN_API uint32_t mc_decoder_get_duration_ms(mc_decoder_t decoder);

/** Decode up to max_samples interleaved signed-16-bit samples into buf.
 *  Returns number of samples decoded, 0 at EOF, -1 on error. */
MUSICCHAIN_API int mc_decoder_read(mc_decoder_t decoder, int16_t* buf, int max_samples);

/** Seek to position in milliseconds. Returns 0 on success. */
MUSICCHAIN_API int mc_decoder_seek(mc_decoder_t decoder, uint32_t position_ms);

/** Get current playback position in milliseconds. */
MUSICCHAIN_API uint32_t mc_decoder_position_ms(mc_decoder_t decoder);

// ---- Checksum -------------------------------------------------------

/** Compute heartbeat checksum over interleaved signed-16-bit samples.
 *  sum = sum of abs(sample) for all samples, result = sum mod 2^32. */
MUSICCHAIN_API uint32_t mc_compute_checksum(const int16_t* samples, int count);

// ---- Fingerprinting -------------------------------------------------

typedef void* mc_fingerprint_t;

/** Generate a Chromaprint fingerprint from raw Ogg data.
 *  Returns a fingerprint handle, or NULL on failure. */
MUSICCHAIN_API mc_fingerprint_t mc_fingerprint_generate(const uint8_t* data, size_t len);

/** Load fingerprint from base64-compressed string.
 *  Returns a fingerprint handle, or NULL on failure. */
MUSICCHAIN_API mc_fingerprint_t mc_fingerprint_from_compressed(const char* base64);

/** Release fingerprint handle. */
MUSICCHAIN_API void mc_fingerprint_free(mc_fingerprint_t fp);

/** Get compressed base64 fingerprint string.
 *  Caller must free with mc_free(). */
MUSICCHAIN_API char* mc_fingerprint_get_compressed(mc_fingerprint_t fp);

/** Compare two fingerprints; returns similarity in [0.0, 1.0]. */
MUSICCHAIN_API float mc_fingerprint_compare(mc_fingerprint_t fp_a, mc_fingerprint_t fp_b);

// ---- Block parsing --------------------------------------------------

/** Find separator (8 x 0xFF bytes) in block data.
 *  Returns byte offset of separator start, or -1 if not found. */
MUSICCHAIN_API int64_t mc_block_find_separator(const uint8_t* data, size_t len);

/** Extract Ogg audio data from a serialized block.
 *  Allocates *ogg_out; caller must free with mc_free().
 *  Returns 0 on success. */
MUSICCHAIN_API int mc_block_extract_audio(const uint8_t* block_data, size_t block_len,
                            uint8_t** ogg_out, size_t* ogg_len_out);

// ---- Cryptographic utilities ----------------------------------------

/** Compute SHA-256 of data and write 32 bytes to out_hash. */
MUSICCHAIN_API void mc_sha256(const uint8_t* data, size_t len, uint8_t* out_hash);

/** Convert bytes to hex string. Caller must free with mc_free(). */
MUSICCHAIN_API char* mc_bytes_to_hex(const uint8_t* data, size_t len);

/** Convert hex string to bytes.
 *  Allocates *out; caller must free with mc_free().
 *  Returns number of bytes, or -1 on error. */
MUSICCHAIN_API int mc_hex_to_bytes(const char* hex, uint8_t** out);

// ---- Audio validation -----------------------------------------------

/** Audio format tag returned by mc_detect_format(). */
#define MC_FORMAT_UNKNOWN 0
#define MC_FORMAT_OGG     1  /**< Ogg/Vorbis or Ogg/Opus */
#define MC_FORMAT_MP3     2

/** Detect the audio format from magic bytes.
 *  Returns one of the MC_FORMAT_* constants. */
MUSICCHAIN_API int mc_detect_format(const uint8_t* data, size_t len);

/** Validate any supported audio file (Ogg/Vorbis, Ogg/Opus, or MP3).
 *  Returns 1 if valid and >= 30 seconds, 0 otherwise.
 *  If error_out is non-null, writes a null-terminated error string (caller frees). */
MUSICCHAIN_API int mc_validate_audio(const uint8_t* data, size_t len, char** error_out);

/** Get duration in ms from any supported audio file. Returns 0 on error. */
MUSICCHAIN_API uint32_t mc_audio_duration_ms(const uint8_t* data, size_t len);

/** Legacy aliases (Ogg only) — kept for backwards compatibility. */
static inline int mc_validate_ogg(const uint8_t* data, size_t len, char** error_out) {
    return mc_validate_audio(data, len, error_out);
}
static inline uint32_t mc_ogg_duration_ms(const uint8_t* data, size_t len) {
    return mc_audio_duration_ms(data, len);
}

#ifdef __cplusplus
} // extern "C"
#endif
