#include "ecies.h"
#include "hash.h"

#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/kdf.h>
#include <openssl/bn.h>
#include <openssl/sha.h>

#include <cstring>

namespace mc::crypto {

namespace {

constexpr uint8_t  kMagic[4]      = { 'M', 'C', 'E', '1' };
constexpr size_t   kHeaderFixed   = 4 + 2 + 8;       // magic + count + plen
constexpr size_t   kRecipientSize = 20 + 33 + 12 + 32 + 16;
constexpr size_t   kBodyTrailer   = 12 + 16;         // body_nonce + body_tag

// ---- Little-endian helpers ------------------------------------------

void put_u16_le(uint8_t* dst, uint16_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFF);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

uint16_t get_u16_le(const uint8_t* src) {
    return static_cast<uint16_t>(src[0]) |
           (static_cast<uint16_t>(src[1]) << 8);
}

void put_u64_le(uint8_t* dst, uint64_t v) {
    for (int i = 0; i < 8; ++i) dst[i] = static_cast<uint8_t>((v >> (8*i)) & 0xFF);
}

uint64_t get_u64_le(const uint8_t* src) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(src[i]) << (8*i);
    return v;
}

// ---- secp256k1 keypair / ECDH wrappers ------------------------------

EC_KEY* make_curve() {
    return EC_KEY_new_by_curve_name(NID_secp256k1);
}

// Generate ephemeral secp256k1 keypair. Caller frees with EC_KEY_free.
EC_KEY* generate_ephemeral() {
    EC_KEY* k = make_curve();
    if (!k) return nullptr;
    if (EC_KEY_generate_key(k) != 1) {
        EC_KEY_free(k);
        return nullptr;
    }
    return k;
}

// Serialize a public key to 33-byte compressed form. Returns true on
// success.
bool pubkey_to_compressed(EC_KEY* k, uint8_t out[33]) {
    const EC_GROUP* g = EC_KEY_get0_group(k);
    const EC_POINT* p = EC_KEY_get0_public_key(k);
    if (!g || !p) return false;
    size_t n = EC_POINT_point2oct(g, p, POINT_CONVERSION_COMPRESSED,
                                  out, 33, nullptr);
    return n == 33;
}

// Build an EC_KEY from a 33-byte compressed pubkey. Caller frees with
// EC_KEY_free.
EC_KEY* pubkey_from_compressed(const uint8_t in[33]) {
    EC_KEY* k = make_curve();
    if (!k) return nullptr;
    const EC_GROUP* g = EC_KEY_get0_group(k);
    EC_POINT* p = EC_POINT_new(g);
    if (!p) { EC_KEY_free(k); return nullptr; }
    if (EC_POINT_oct2point(g, p, in, 33, nullptr) != 1 ||
        EC_KEY_set_public_key(k, p) != 1) {
        EC_POINT_free(p);
        EC_KEY_free(k);
        return nullptr;
    }
    EC_POINT_free(p);
    return k;
}

// Build an EC_KEY from a 32-byte raw secret. Caller frees with
// EC_KEY_free.
EC_KEY* privkey_from_raw(const uint8_t in[32]) {
    EC_KEY* k = make_curve();
    if (!k) return nullptr;
    BIGNUM* bn = BN_bin2bn(in, 32, nullptr);
    if (!bn) { EC_KEY_free(k); return nullptr; }
    if (EC_KEY_set_private_key(k, bn) != 1) {
        BN_free(bn);
        EC_KEY_free(k);
        return nullptr;
    }
    // Compute the matching public point so EVP_PKEY_derive will work.
    const EC_GROUP* g = EC_KEY_get0_group(k);
    EC_POINT* pub = EC_POINT_new(g);
    if (!pub) { BN_free(bn); EC_KEY_free(k); return nullptr; }
    if (EC_POINT_mul(g, pub, bn, nullptr, nullptr, nullptr) != 1 ||
        EC_KEY_set_public_key(k, pub) != 1) {
        EC_POINT_free(pub);
        BN_free(bn);
        EC_KEY_free(k);
        return nullptr;
    }
    EC_POINT_free(pub);
    BN_free(bn);
    return k;
}

// Perform ECDH between `our_key` (with private + public) and `peer_pub`
// and write the 32-byte X-coordinate of the shared point to `out`.
bool ecdh_shared(EC_KEY* our_key, EC_KEY* peer_pub, uint8_t out[32]) {
    EVP_PKEY* our_evp = EVP_PKEY_new();
    EVP_PKEY* peer_evp = EVP_PKEY_new();
    if (!our_evp || !peer_evp) {
        if (our_evp)  EVP_PKEY_free(our_evp);
        if (peer_evp) EVP_PKEY_free(peer_evp);
        return false;
    }
    EVP_PKEY_set1_EC_KEY(our_evp, our_key);
    EVP_PKEY_set1_EC_KEY(peer_evp, peer_pub);

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(our_evp, nullptr);
    bool ok = false;
    do {
        if (!ctx) break;
        if (EVP_PKEY_derive_init(ctx) <= 0) break;
        if (EVP_PKEY_derive_set_peer(ctx, peer_evp) <= 0) break;
        size_t out_len = 32;
        if (EVP_PKEY_derive(ctx, out, &out_len) <= 0) break;
        if (out_len != 32) break;
        ok = true;
    } while (false);

    if (ctx) EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(our_evp);
    EVP_PKEY_free(peer_evp);
    return ok;
}

// HKDF-SHA256(shared_secret, salt=recipient_address, info="mc-ecies-v1")
// → 32-byte key-wrap key.
bool derive_wrap_key(const uint8_t shared[32],
                     const uint8_t addr[20],
                     uint8_t        out[32]) {
    static const uint8_t kInfo[] = "mc-ecies-v1";
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!ctx) return false;
    bool ok = false;
    do {
        if (EVP_PKEY_derive_init(ctx) <= 0) break;
        if (EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) <= 0) break;
        if (EVP_PKEY_CTX_set1_hkdf_key(ctx, shared, 32) <= 0) break;
        if (EVP_PKEY_CTX_set1_hkdf_salt(ctx, addr, 20) <= 0) break;
        if (EVP_PKEY_CTX_add1_hkdf_info(ctx, kInfo, sizeof(kInfo) - 1) <= 0) break;
        size_t len = 32;
        if (EVP_PKEY_derive(ctx, out, &len) <= 0) break;
        ok = (len == 32);
    } while (false);
    EVP_PKEY_CTX_free(ctx);
    return ok;
}

// AES-256-GCM encrypt. `out_ct` must have room for `pt_len` bytes;
// `tag` gets 16 bytes. `aad` may be null when aad_len == 0.
bool aes_gcm_encrypt(const uint8_t key[32], const uint8_t iv[12],
                     const uint8_t* aad, size_t aad_len,
                     const uint8_t* pt,  size_t pt_len,
                     uint8_t* out_ct,    uint8_t tag[16]) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    bool ok = false;
    int  outl = 0;
    do {
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key, iv) != 1) break;
        if (aad_len) {
            if (EVP_EncryptUpdate(ctx, nullptr, &outl, aad,
                                  static_cast<int>(aad_len)) != 1) break;
        }
        if (pt_len) {
            if (EVP_EncryptUpdate(ctx, out_ct, &outl, pt,
                                  static_cast<int>(pt_len)) != 1) break;
        }
        int finall = 0;
        if (EVP_EncryptFinal_ex(ctx, out_ct + outl, &finall) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1) break;
        ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

bool aes_gcm_decrypt(const uint8_t key[32], const uint8_t iv[12],
                     const uint8_t* aad, size_t aad_len,
                     const uint8_t* ct,  size_t ct_len,
                     const uint8_t tag[16],
                     uint8_t* out_pt) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    bool ok = false;
    int  outl = 0;
    do {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key, iv) != 1) break;
        if (aad_len) {
            if (EVP_DecryptUpdate(ctx, nullptr, &outl, aad,
                                  static_cast<int>(aad_len)) != 1) break;
        }
        if (ct_len) {
            if (EVP_DecryptUpdate(ctx, out_pt, &outl, ct,
                                  static_cast<int>(ct_len)) != 1) break;
        }
        // Casting away const is required by the legacy OpenSSL signature.
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
                                const_cast<uint8_t*>(tag)) != 1) break;
        int finall = 0;
        ok = EVP_DecryptFinal_ex(ctx, out_pt + outl, &finall) == 1;
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

} // namespace

// ---- Public API -----------------------------------------------------

bool ecies_looks_encrypted(const uint8_t* data, size_t len) {
    return len >= 4 && std::memcmp(data, kMagic, 4) == 0;
}

std::vector<uint8_t> ecies_encrypt(
    const std::vector<uint8_t>& plaintext,
    const std::vector<std::pair<Address, PubKey33>>& recipients) {
    if (recipients.empty()) return {};
    if (recipients.size() > 0xFFFF) return {};

    // Generate the content-encryption key + body nonce.
    uint8_t cek[32];
    uint8_t body_nonce[12];
    if (RAND_bytes(cek, 32) != 1) return {};
    if (RAND_bytes(body_nonce, 12) != 1) return {};

    std::vector<uint8_t> body_ct(plaintext.size());
    uint8_t body_tag[16];
    if (!aes_gcm_encrypt(cek, body_nonce, nullptr, 0,
                         plaintext.data(), plaintext.size(),
                         body_ct.data(), body_tag)) {
        return {};
    }

    // Pre-allocate the wire blob.
    const size_t blob_size = kHeaderFixed
                           + recipients.size() * kRecipientSize
                           + kBodyTrailer + plaintext.size();
    std::vector<uint8_t> out(blob_size);
    uint8_t* p = out.data();

    std::memcpy(p, kMagic, 4);                                          p += 4;
    put_u16_le(p, static_cast<uint16_t>(recipients.size()));            p += 2;
    put_u64_le(p, static_cast<uint64_t>(plaintext.size()));             p += 8;

    // Per-recipient wrap slots.
    for (const auto& [addr, pubkey] : recipients) {
        EC_KEY* eph    = generate_ephemeral();
        EC_KEY* peer   = pubkey_from_compressed(pubkey.data());
        if (!eph || !peer) {
            if (eph) EC_KEY_free(eph);
            if (peer) EC_KEY_free(peer);
            return {};
        }
        uint8_t eph_pub[33];
        if (!pubkey_to_compressed(eph, eph_pub)) {
            EC_KEY_free(eph); EC_KEY_free(peer); return {};
        }

        uint8_t shared[32];
        if (!ecdh_shared(eph, peer, shared)) {
            EC_KEY_free(eph); EC_KEY_free(peer); return {};
        }
        EC_KEY_free(peer);
        EC_KEY_free(eph);

        uint8_t wrap_key[32];
        if (!derive_wrap_key(shared, addr.data(), wrap_key)) return {};

        uint8_t wrap_nonce[12];
        if (RAND_bytes(wrap_nonce, 12) != 1) return {};

        uint8_t wrapped_cek[32];
        uint8_t wrap_tag[16];
        if (!aes_gcm_encrypt(wrap_key, wrap_nonce, nullptr, 0,
                             cek, 32, wrapped_cek, wrap_tag)) {
            return {};
        }

        std::memcpy(p, addr.data(),       20); p += 20;
        std::memcpy(p, eph_pub,           33); p += 33;
        std::memcpy(p, wrap_nonce,        12); p += 12;
        std::memcpy(p, wrapped_cek,       32); p += 32;
        std::memcpy(p, wrap_tag,          16); p += 16;
    }

    // Body
    std::memcpy(p, body_nonce, 12);                                     p += 12;
    std::memcpy(p, body_ct.data(), body_ct.size());                     p += body_ct.size();
    std::memcpy(p, body_tag, 16);                                       p += 16;

    // Wipe CEK
    OPENSSL_cleanse(cek, sizeof(cek));
    return out;
}

std::optional<std::vector<uint8_t>> ecies_decrypt(
    const std::vector<uint8_t>& ct,
    const Address& my_addr,
    const std::vector<uint8_t>& my_priv_key) {
    if (my_priv_key.size() != 32) return std::nullopt;
    if (ct.size() < kHeaderFixed) return std::nullopt;
    if (std::memcmp(ct.data(), kMagic, 4) != 0) return std::nullopt;

    const uint16_t n_rec = get_u16_le(ct.data() + 4);
    const uint64_t plen  = get_u64_le(ct.data() + 6);
    if (n_rec == 0) return std::nullopt;

    const size_t need = kHeaderFixed
                      + static_cast<size_t>(n_rec) * kRecipientSize
                      + kBodyTrailer + static_cast<size_t>(plen);
    if (ct.size() < need) return std::nullopt;

    // Find our slot.
    const uint8_t* slots = ct.data() + kHeaderFixed;
    int slot_idx = -1;
    for (uint16_t i = 0; i < n_rec; ++i) {
        const uint8_t* s = slots + i * kRecipientSize;
        if (std::memcmp(s, my_addr.data(), 20) == 0) { slot_idx = i; break; }
    }
    if (slot_idx < 0) return std::nullopt;

    const uint8_t* s         = slots + slot_idx * kRecipientSize;
    const uint8_t* eph_pub   = s + 20;
    const uint8_t* wrap_n    = eph_pub + 33;
    const uint8_t* wrapped   = wrap_n + 12;
    const uint8_t* wrap_tag  = wrapped + 32;

    // ECDH(our_priv, ephemeral_pub) → shared
    EC_KEY* our    = privkey_from_raw(my_priv_key.data());
    EC_KEY* peer   = pubkey_from_compressed(eph_pub);
    if (!our || !peer) {
        if (our) EC_KEY_free(our);
        if (peer) EC_KEY_free(peer);
        return std::nullopt;
    }
    uint8_t shared[32];
    bool dh_ok = ecdh_shared(our, peer, shared);
    EC_KEY_free(peer);
    EC_KEY_free(our);
    if (!dh_ok) return std::nullopt;

    uint8_t wrap_key[32];
    if (!derive_wrap_key(shared, my_addr.data(), wrap_key)) return std::nullopt;
    OPENSSL_cleanse(shared, sizeof(shared));

    uint8_t cek[32];
    if (!aes_gcm_decrypt(wrap_key, wrap_n, nullptr, 0,
                         wrapped, 32, wrap_tag, cek)) {
        OPENSSL_cleanse(wrap_key, sizeof(wrap_key));
        return std::nullopt;
    }
    OPENSSL_cleanse(wrap_key, sizeof(wrap_key));

    // Body
    const uint8_t* body_start = slots + n_rec * kRecipientSize;
    const uint8_t* body_nonce = body_start;
    const uint8_t* body_ct    = body_nonce + 12;
    const uint8_t* body_tag   = body_ct + plen;

    std::vector<uint8_t> pt(static_cast<size_t>(plen));
    bool ok = aes_gcm_decrypt(cek, body_nonce, nullptr, 0,
                              body_ct, static_cast<size_t>(plen), body_tag,
                              pt.data());
    OPENSSL_cleanse(cek, sizeof(cek));
    if (!ok) return std::nullopt;
    return pt;
}

} // namespace mc::crypto
