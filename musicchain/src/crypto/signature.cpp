#include "signature.h"
#include "hash.h"
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>
#include <openssl/bn.h>
#include <stdexcept>
#include <cstring>

namespace mc::crypto {

namespace {

EC_KEY* ec_key_from_priv(const std::vector<uint8_t>& priv_key) {
    EC_KEY*   key   = EC_KEY_new_by_curve_name(NID_secp256k1);
    BIGNUM*   bn    = BN_bin2bn(priv_key.data(), 32, nullptr);
    const EC_GROUP* group = EC_KEY_get0_group(key);
    EC_POINT* pub = EC_POINT_new(group);
    EC_KEY_set_private_key(key, bn);
    EC_POINT_mul(group, pub, bn, nullptr, nullptr, nullptr);
    EC_KEY_set_public_key(key, pub);
    EC_POINT_free(pub);
    BN_free(bn);
    return key;
}

EC_KEY* ec_key_from_pubkey(const PubKey33& pubkey) {
    EC_KEY*   key   = EC_KEY_new_by_curve_name(NID_secp256k1);
    const EC_GROUP* group = EC_KEY_get0_group(key);
    EC_POINT* pub = EC_POINT_new(group);
    if (!EC_POINT_oct2point(group, pub, pubkey.data(), 33, nullptr)) {
        EC_POINT_free(pub);
        EC_KEY_free(key);
        return nullptr;
    }
    EC_KEY_set_public_key(key, pub);
    EC_POINT_free(pub);
    return key;
}

// Pack ECDSA_SIG into 64 bytes (r || s, each 32 bytes big-endian)
bool sig_to_compact(ECDSA_SIG* ecdsa_sig, Sig64& out) {
    const BIGNUM* r;
    const BIGNUM* s;
    ECDSA_SIG_get0(ecdsa_sig, &r, &s);
    int rlen = BN_num_bytes(r), slen = BN_num_bytes(s);
    if (rlen > 32 || slen > 32) return false;
    out.fill(0);
    BN_bn2bin(r, out.data() + (32 - rlen));
    BN_bn2bin(s, out.data() + 32 + (32 - slen));
    return true;
}

ECDSA_SIG* compact_to_sig(const Sig64& sig) {
    ECDSA_SIG* ecdsa_sig = ECDSA_SIG_new();
    BIGNUM* r = BN_bin2bn(sig.data(),      32, nullptr);
    BIGNUM* s = BN_bin2bn(sig.data() + 32, 32, nullptr);
    ECDSA_SIG_set0(ecdsa_sig, r, s);
    return ecdsa_sig;
}

} // anonymous namespace

Sig64 sign_ecdsa(const Hash256& hash, const std::vector<uint8_t>& priv_key) {
    EC_KEY* key = ec_key_from_priv(priv_key);
    ECDSA_SIG* ecdsa_sig = ECDSA_do_sign(hash.data(), 32, key);
    EC_KEY_free(key);
    if (!ecdsa_sig) throw std::runtime_error("ECDSA signing failed");
    Sig64 out;
    if (!sig_to_compact(ecdsa_sig, out)) {
        ECDSA_SIG_free(ecdsa_sig);
        throw std::runtime_error("signature packing failed");
    }
    ECDSA_SIG_free(ecdsa_sig);
    return out;
}

bool verify_ecdsa(const Hash256& hash, const Sig64& sig, const PubKey33& pubkey) {
    EC_KEY* key = ec_key_from_pubkey(pubkey);
    if (!key) return false;
    ECDSA_SIG* ecdsa_sig = compact_to_sig(sig);
    int result = ECDSA_do_verify(hash.data(), 32, ecdsa_sig, key);
    ECDSA_SIG_free(ecdsa_sig);
    EC_KEY_free(key);
    return result == 1;
}

bool verify_ecdsa_from_address(const Hash256& hash, const Sig64& sig,
                                const Address& expected_address) {
    // DEAD STUB — ALWAYS RETURNS FALSE. ECDSA public-key recovery isn't
    // available in the OpenSSL-only crypto layer, so verifying a
    // signature from an address alone (without the pubkey) is impossible
    // here. Live code MUST NOT use this: every signed transaction now
    // carries its signer's compressed pubkey inline and verifies via
    // verify_ecdsa() with an address cross-check (see TransferTx and the
    // other *Tx::verify_signature). The only remaining caller is the
    // legacy HTTP moderator route verify_moderator_sig(), which is
    // unreachable (no HTTP listener post-pivot) and slated for deletion.
    (void)hash; (void)sig; (void)expected_address;
    return false;
}

Sig64 sign_data(const uint8_t* data, size_t len, const std::vector<uint8_t>& priv_key) {
    auto hash = sha256(data, len);
    return sign_ecdsa(hash, priv_key);
}

bool verify_data(const uint8_t* data, size_t len, const Sig64& sig,
                 const PubKey33& pubkey) {
    auto hash = sha256(data, len);
    return verify_ecdsa(hash, sig, pubkey);
}

} // namespace mc::crypto
