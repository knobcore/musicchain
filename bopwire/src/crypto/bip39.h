#pragma once
#include "keys.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mc::crypto {

// BIP39 mnemonic helpers (English wordlist only).
//
// Used by every non-founder wallet creation flow: the player generates
// a fresh wallet, derives it from a freshly-randomized 128-bit entropy
// payload, encodes that as a 12-word mnemonic, and displays the words
// for the user to back up. Restoring is the inverse — type the 12
// words in order, recover the same secp256k1 keypair.
//
// The founder bootstrap path stays on PBKDF2(passphrase) — see
// derive_seed_pbkdf2_sha512 — because the founder identifies by
// memorised passphrase rather than a written-down phrase.

// Generate a fresh 12-word BIP39 mnemonic using `RAND_bytes` for the
// underlying 128-bit entropy. Returns the lowercased space-separated
// phrase. Returns empty string if entropy generation fails (should
// never happen on a healthy host).
std::string bip39_generate_12();

// Validate a candidate mnemonic. Returns true iff:
//   * every word is in the BIP39 English wordlist
//   * the word count is one of {12, 15, 18, 21, 24}
//   * the embedded checksum matches
bool bip39_validate(const std::string& mnemonic);

// Derive the 64-byte BIP39 seed from a mnemonic + optional passphrase
// (PBKDF2-HMAC-SHA512, 2048 iterations, salt = "mnemonic" + passphrase
// as per the spec). Returns an empty vector if the mnemonic fails
// validation.
std::vector<uint8_t> bip39_mnemonic_to_seed(const std::string& mnemonic,
                                            const std::string& passphrase = "");

// Derive a usable secp256k1 keypair from a mnemonic. We deliberately
// SKIP the full BIP32 m/44'/0'/0'/0/0 derivation chain — the chain
// uses 20-byte address-from-pubkey identifiers, not Bitcoin-style
// hardened paths — so the first 32 bytes of the BIP39 seed become the
// private key directly. This gives a single deterministic key per
// mnemonic + passphrase pair.
std::optional<KeyPair> bip39_mnemonic_to_keypair(
    const std::string& mnemonic, const std::string& passphrase = "");

} // namespace mc::crypto
