#pragma once
#include <cstdint>
#include "../core/block.h"
#include "keys.h"

namespace mc::crypto {

// Sign a 32-byte hash with the private key; returns 64-byte compact DER-less sig
Sig64 sign_ecdsa(const Hash256& hash, const std::vector<uint8_t>& priv_key);

// Verify a 64-byte signature against hash and compressed public key
bool verify_ecdsa(const Hash256& hash, const Sig64& sig, const PubKey33& pubkey);

// Verify against an address (derives address from recovered pubkey)
bool verify_ecdsa_from_address(const Hash256& hash, const Sig64& sig,
                                const Address& expected_address);

// Sign arbitrary data (hashes internally, then signs)
Sig64 sign_data(const uint8_t* data, size_t len, const std::vector<uint8_t>& priv_key);
bool  verify_data(const uint8_t* data, size_t len, const Sig64& sig,
                  const PubKey33& pubkey);

} // namespace mc::crypto
