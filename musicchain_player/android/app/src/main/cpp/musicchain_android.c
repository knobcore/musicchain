/**
 * musicchain_android.c
 * Android FFI implementation of the player-facing slice of musicchain.h.
 *
 * Wallet derivation is libwally-backed (BIP39 12-word mnemonic → BIP32
 * m/44'/19779'/0'/0/0 → secp256k1 keypair). Address derivation is
 * keccak256(uncompressed_pubkey[1..65])[12..32] with EIP-55 mixed case
 * checksum — bit-identical to what the Windows DLL does, so the same
 * mnemonic on both devices produces the same address.
 *
 * The legacy mc_wallet_create / mc_wallet_import paths are gone; the
 * only wallet creation route is mc_wallet_from_mnemonic. Existing wallet
 * files saved before this version were 32 raw bytes of private key, so
 * mc_wallet_load still understands them.
 */

#include "musicchain.h"
#include "secp256k1.h"
#include "keccak256_c.h"

/* libwally — vendored at ../../../../../../musicchain/deps/libwally-core */
#include <wally_core.h>
#include <wally_bip39.h>
#include <wally_bip32.h>
#include <wally_crypto.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <ctype.h>

/* ---- Hex helpers ------------------------------------------------------ */
static const char hex_chars[] = "0123456789abcdef";

static char* bytes_to_hex_alloc(const uint8_t* d, size_t n) {
    char* s = (char*)malloc(n*2 + 1);
    size_t i;
    if (!s) return NULL;
    for (i = 0; i < n; ++i) {
        s[i*2]   = hex_chars[d[i] >> 4];
        s[i*2+1] = hex_chars[d[i] & 0xF];
    }
    s[n*2] = '\0';
    return s;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ---- secp256k1 + libwally init --------------------------------------- */
static secp256k1_context* g_ctx       = NULL;
static int                g_wally_ok  = 0;

int mc_init(void) {
    if (!g_ctx)
        g_ctx = secp256k1_context_create(
                    SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    if (!g_wally_ok && wally_init(0) == WALLY_OK) g_wally_ok = 1;
    return g_ctx ? 0 : 1;
}
void mc_cleanup(void) {
    if (g_ctx) { secp256k1_context_destroy(g_ctx); g_ctx = NULL; }
    if (g_wally_ok) { wally_cleanup(0); g_wally_ok = 0; }
}
void        mc_free(void* p)        { free(p); }
const char* mc_last_error(void)     { return NULL; }

/* ---- Address derivation (keccak256, EIP-55) -------------------------- */

typedef struct { uint8_t priv[32]; } WalletHandle;

static void derive_address_keccak(const uint8_t priv[32], uint8_t addr_out[20]) {
    secp256k1_pubkey pub;
    uint8_t uncompressed[65];
    size_t outlen = 65;
    uint8_t hash[32];
    secp256k1_ec_pubkey_create(g_ctx, &pub, priv);
    secp256k1_ec_pubkey_serialize(g_ctx, uncompressed, &outlen, &pub,
                                  SECP256K1_EC_UNCOMPRESSED);
    /* Ethereum-style: keccak256 of the X || Y bytes (drop 0x04 prefix),
     * take the last 20 bytes. */
    mc_keccak256(uncompressed + 1, 64, hash);
    memcpy(addr_out, hash + 12, 20);
}

static void get_compressed_pubkey(const uint8_t priv[32], uint8_t pub_out[33]) {
    secp256k1_pubkey pub;
    size_t outlen = 33;
    secp256k1_ec_pubkey_create(g_ctx, &pub, priv);
    secp256k1_ec_pubkey_serialize(g_ctx, pub_out, &outlen, &pub,
                                  SECP256K1_EC_COMPRESSED);
}

/* EIP-55 mixed-case checksum. addr_hex is 40 hex chars (lowercase) in,
 * 40 hex chars (mixed) out. Standard Ethereum / Base format. */
static void eip55_checksum(char addr_hex[41]) {
    uint8_t hash[32];
    int i;
    /* Lowercase the input first. */
    for (i = 0; i < 40; ++i) {
        if (addr_hex[i] >= 'A' && addr_hex[i] <= 'F')
            addr_hex[i] = (char)(addr_hex[i] + ('a' - 'A'));
    }
    /* hash the ASCII lowercase address (no 0x). */
    mc_keccak256((const uint8_t*)addr_hex, 40, hash);
    for (i = 0; i < 40; ++i) {
        if (addr_hex[i] >= 'a' && addr_hex[i] <= 'f') {
            /* nibble of hash byte i/2 — high nibble for even i, low for odd. */
            uint8_t nibble = (i & 1) ? (hash[i/2] & 0x0F) : (hash[i/2] >> 4);
            if (nibble >= 8) addr_hex[i] = (char)(addr_hex[i] - ('a' - 'A'));
        }
    }
}

/* ---- Wallet ----------------------------------------------------------- */

/* mc_wallet_create / mc_wallet_import — DELETED.
 *
 * New wallets are created via mc_wallet_from_mnemonic. Importing a raw
 * hex private key was a debug-time path we no longer expose; users who
 * need to bring a key in supply the mnemonic that derives it. */

mc_wallet_t mc_wallet_load(const char* path, const char* password) {
    WalletHandle* w;
    FILE* f;
    size_t n;
    (void)password;
    if (!path) return NULL;
    f = fopen(path, "rb");
    if (!f) return NULL;
    w = (WalletHandle*)malloc(sizeof(WalletHandle));
    if (!w) { fclose(f); return NULL; }
    n = fread(w->priv, 1, 32, f);
    fclose(f);
    if (n != 32 || !secp256k1_ec_seckey_verify(g_ctx, w->priv)) {
        free(w); return NULL;
    }
    return w;
}

int mc_wallet_save(mc_wallet_t wallet, const char* path) {
    WalletHandle* w = (WalletHandle*)wallet;
    FILE* f;
    if (!w || !path) return -1;
    f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(w->priv, 1, 32, f);
    fclose(f);
    return 0;
}

void mc_wallet_free(mc_wallet_t wallet) { free(wallet); }

char* mc_wallet_get_address(mc_wallet_t wallet) {
    WalletHandle* w = (WalletHandle*)wallet;
    uint8_t addr[20];
    if (!w) return NULL;
    derive_address_keccak(w->priv, addr);
    return bytes_to_hex_alloc(addr, 20);
}

char* mc_wallet_get_public_key(mc_wallet_t wallet) {
    WalletHandle* w = (WalletHandle*)wallet;
    uint8_t pub[33];
    if (!w) return NULL;
    get_compressed_pubkey(w->priv, pub);
    return bytes_to_hex_alloc(pub, 33);
}

char* mc_wallet_sign(mc_wallet_t wallet, const uint8_t* data, size_t len) {
    WalletHandle* w = (WalletHandle*)wallet;
    uint8_t hash[32];
    secp256k1_ecdsa_signature sig;
    uint8_t compact[64];
    if (!w) return NULL;
    /* The home-node sign path hashes with sha256 (NOT keccak); see
     * src/crypto/signature.cpp. We use libwally's sha256 to avoid
     * pulling in a separate implementation. */
    {
        size_t written = 0;
        if (wally_sha256(data, len, hash, sizeof(hash)) != WALLY_OK) return NULL;
        (void)written;
    }
    if (!secp256k1_ecdsa_sign(g_ctx, &sig, hash, w->priv, NULL, NULL))
        return NULL;
    secp256k1_ecdsa_signature_normalize(g_ctx, &sig, &sig);
    secp256k1_ecdsa_signature_serialize_compact(g_ctx, compact, &sig);
    return bytes_to_hex_alloc(compact, 64);
}

/* ---- BIP39 + BIP32 derivation via libwally --------------------------- */

char* mc_bip39_generate_12(void) {
    unsigned char entropy[16];
    struct words* word_list = NULL;
    char* mnemonic = NULL;
    FILE* urand;
    /* wally_secp_randomize CONSUMES caller-supplied entropy to harden
     * the internal secp256k1 context — it does NOT produce randomness.
     * wally also doesn't ship a get-random-bytes API. So we read from
     * /dev/urandom directly. Android always exposes it. */
    urand = fopen("/dev/urandom", "rb");
    if (!urand) return NULL;
    if (fread(entropy, 1, sizeof(entropy), urand) != sizeof(entropy)) {
        fclose(urand); return NULL;
    }
    fclose(urand);
    (void)wally_secp_randomize(entropy, sizeof(entropy));
    if (bip39_get_wordlist(NULL, &word_list) != WALLY_OK) return NULL;
    if (bip39_mnemonic_from_bytes(word_list, entropy, sizeof(entropy),
                                  &mnemonic) != WALLY_OK) return NULL;
    /* libwally returns a wally-allocated string; copy into malloc so the
     * caller can free with mc_free. */
    if (mnemonic) {
        size_t n = strlen(mnemonic);
        char* out = (char*)malloc(n + 1);
        if (out) memcpy(out, mnemonic, n + 1);
        wally_free_string(mnemonic);
        return out;
    }
    return NULL;
}

int mc_bip39_validate(const char* mnemonic) {
    struct words* word_list = NULL;
    if (!mnemonic) return 0;
    if (bip39_get_wordlist(NULL, &word_list) != WALLY_OK) return 0;
    return bip39_mnemonic_validate(word_list, mnemonic) == WALLY_OK ? 1 : 0;
}

mc_wallet_t mc_wallet_from_mnemonic(const char* mnemonic,
                                     const char* passphrase) {
    unsigned char seed[BIP39_SEED_LEN_512];
    size_t        seed_written = 0;
    struct ext_key master_key, child;
    uint32_t      path[5];
    WalletHandle* w;
    if (!mnemonic) return NULL;
    if (bip39_mnemonic_to_seed(mnemonic,
                                passphrase ? passphrase : "",
                                seed, sizeof(seed),
                                &seed_written) != WALLY_OK) return NULL;
    if (seed_written != BIP39_SEED_LEN_512) return NULL;
    if (bip32_key_from_seed(seed, seed_written,
                             BIP32_VER_MAIN_PRIVATE, 0,
                             &master_key) != WALLY_OK) return NULL;
    /* m / 44' / 19779' / 0' / 0 / 0 — same SLIP-44 coin index the home
     * node uses, so the same mnemonic derives the same secp256k1 key on
     * every platform. */
    path[0] = 44u    | BIP32_INITIAL_HARDENED_CHILD;
    path[1] = 19779u | BIP32_INITIAL_HARDENED_CHILD;
    path[2] = 0u     | BIP32_INITIAL_HARDENED_CHILD;
    path[3] = 0u;
    path[4] = 0u;
    if (bip32_key_from_parent_path(&master_key, path, 5,
                                    BIP32_FLAG_KEY_PRIVATE,
                                    &child) != WALLY_OK) return NULL;
    w = (WalletHandle*)malloc(sizeof(WalletHandle));
    if (!w) return NULL;
    /* child.priv_key[0] is the 0x00 prefix byte; the 32-byte scalar is
     * priv_key[1..33]. */
    memcpy(w->priv, child.priv_key + 1, 32);
    if (!secp256k1_ec_seckey_verify(g_ctx, w->priv)) { free(w); return NULL; }
    return w;
}

char* mc_wallet_get_eth_address(mc_wallet_t wallet) {
    WalletHandle* w = (WalletHandle*)wallet;
    uint8_t addr[20];
    char buf[43]; /* "0x" + 40 hex + NUL */
    char* out;
    size_t i;
    if (!w) return NULL;
    derive_address_keccak(w->priv, addr);
    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 20; ++i) {
        buf[2 + i*2]     = hex_chars[addr[i] >> 4];
        buf[2 + i*2 + 1] = hex_chars[addr[i] & 0xF];
    }
    buf[42] = '\0';
    /* EIP-55 operates on the hex chars only, not the 0x prefix. */
    eip55_checksum(buf + 2);
    out = (char*)malloc(43);
    if (!out) return NULL;
    memcpy(out, buf, 43);
    return out;
}

/* ---- SHA256 (still needed for mc_compute_checksum + mc_sha256) ------ */

void mc_sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    if (wally_sha256(data, len, out, 32) != WALLY_OK) {
        memset(out, 0, 32);
    }
}

int mc_hex_to_bytes(const char* hex, uint8_t** out) {
    size_t hlen, blen, i;
    uint8_t* buf;
    if (!hex || !out) return -1;
    hlen = strlen(hex);
    if (hex[0] == '0' && hex[1] == 'x') { hex += 2; hlen -= 2; }
    if (hlen & 1) return -1;
    blen = hlen / 2;
    buf = (uint8_t*)malloc(blen);
    if (!buf) return -1;
    for (i = 0; i < blen; ++i) {
        int hi = hex_nibble(hex[i*2]);
        int lo = hex_nibble(hex[i*2 + 1]);
        if (hi < 0 || lo < 0) { free(buf); return -1; }
        buf[i] = (uint8_t)((hi << 4) | lo);
    }
    *out = buf;
    return (int)blen;
}

/* ---- Checksum --------------------------------------------------------- */
uint32_t mc_compute_checksum(const int16_t* samples, int count) {
    uint32_t h = 0;
    int i;
    for (i = 0; i < count; ++i) {
        h = (h * 31u) + (uint32_t)samples[i];
    }
    return h;
}

/* ---- Audio / fingerprint stubs (player handles audio in Dart) -------
 *
 * Signatures mirror include/musicchain.h exactly. The mc_validate_ogg /
 * mc_ogg_duration_ms exports live as `static inline` in the header and
 * MUST NOT appear here — defining them would collide with the inline
 * version the Dart FFI bindings.dart pulls in.
 */
mc_decoder_t  mc_decoder_open(const uint8_t* d, size_t l)             { (void)d; (void)l; return NULL; }
void          mc_decoder_free(mc_decoder_t dc)                        { (void)dc; }
int           mc_decoder_get_sample_rate(mc_decoder_t dc)             { (void)dc; return 0; }
int           mc_decoder_get_channels(mc_decoder_t dc)                { (void)dc; return 0; }
uint32_t      mc_decoder_get_duration_ms(mc_decoder_t dc)             { (void)dc; return 0; }
int           mc_decoder_read(mc_decoder_t dc, int16_t* b, int n)     { (void)dc; (void)b; (void)n; return -1; }
int           mc_decoder_seek(mc_decoder_t dc, uint32_t p)            { (void)dc; (void)p; return -1; }
uint32_t      mc_decoder_position_ms(mc_decoder_t dc)                 { (void)dc; return 0; }
mc_fingerprint_t mc_fingerprint_generate(const uint8_t* d, size_t l)  { (void)d; (void)l; return NULL; }
mc_fingerprint_t mc_fingerprint_from_compressed(const char* b)        { (void)b; return NULL; }
void          mc_fingerprint_free(mc_fingerprint_t fp)                { (void)fp; }
char*         mc_fingerprint_get_compressed(mc_fingerprint_t fp)      { (void)fp; return NULL; }
float         mc_fingerprint_compare(mc_fingerprint_t a, mc_fingerprint_t b) { (void)a; (void)b; return 0.0f; }
int64_t       mc_block_find_separator(const uint8_t* b, size_t l)     { (void)b; (void)l; return -1; }
int           mc_block_extract_audio(const uint8_t* bd, size_t bl, uint8_t** o, size_t* ol) { (void)bd; (void)bl; (void)o; (void)ol; return -1; }
char*         mc_bytes_to_hex(const uint8_t* d, size_t n)             { return bytes_to_hex_alloc(d, n); }
int           mc_detect_format(const uint8_t* d, size_t l)            { (void)d; (void)l; return 0; }
int           mc_validate_audio(const uint8_t* d, size_t l, char** e) { (void)d; (void)l; (void)e; return 0; }
uint32_t      mc_audio_duration_ms(const uint8_t* d, size_t l)        { (void)d; (void)l; return 0; }
