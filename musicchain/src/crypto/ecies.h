#pragma once
#include "../core/block.h"
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace mc::crypto {

// Multi-recipient ECIES on secp256k1 + AES-256-GCM.
//
// Used by the DMCA/KYC inbox writers on the home node to encrypt every
// incoming form against the union of currently-active moderators'
// secp256k1 public keys. Any moderator on that list can later decrypt
// the file with their own private key alone; everyone else (including
// the node operator if they aren't a mod) sees only opaque ciphertext.
//
// Wire format (little-endian, single contiguous blob):
//
//   magic "MCE1"           (4 bytes)
//   recipient_count        (uint16_t, LE)
//   plaintext_len          (uint64_t, LE; pre-encryption byte count)
//   for each recipient:
//       address            (20 bytes)
//       ephemeral_pubkey   (33 bytes, compressed secp256k1)
//       wrap_nonce         (12 bytes, AES-GCM IV for CEK wrap)
//       wrapped_cek        (32 bytes, AES-256-GCM ciphertext of CEK)
//       wrap_tag           (16 bytes)
//   body_nonce             (12 bytes, AES-GCM IV for the payload)
//   body_ciphertext        (plaintext_len bytes)
//   body_tag               (16 bytes)
//
// Per-recipient size: 20 + 33 + 12 + 32 + 16 = 113 bytes.
//
// Security properties (single-pass, no forward secrecy beyond the
// ephemeral key):
//   * Confidentiality from anyone without a key on the recipient list.
//   * Authenticity per-recipient: a tampered ciphertext fails GCM auth.
//   * Recipient unlinkability is NOT a goal — the recipient address
//     is in the header so the decrypt side can find its own slot in
//     constant time without trial-decrypting every wrap slot.

/// Encrypt `plaintext` to every recipient in `recipients`. Returns the
/// serialized ciphertext blob on success, empty vector on failure (eg
/// the recipient list is empty or contains a malformed pubkey).
std::vector<uint8_t> ecies_encrypt(
    const std::vector<uint8_t>& plaintext,
    const std::vector<std::pair<Address, PubKey33>>& recipients);

/// Decrypt an ECIES blob. Returns the plaintext if `my_addr` is one of
/// the recipients embedded in the blob and `my_priv_key` is the
/// matching secp256k1 private key. Returns nullopt otherwise (not a
/// recipient, malformed blob, GCM tag mismatch, etc.).
std::optional<std::vector<uint8_t>> ecies_decrypt(
    const std::vector<uint8_t>& ciphertext,
    const Address& my_addr,
    const std::vector<uint8_t>& my_priv_key);

/// Returns true if `data` starts with the ECIES magic bytes. Used by
/// the TUI to display ".enc" markers in the inbox listing.
bool ecies_looks_encrypted(const uint8_t* data, size_t len);

} // namespace mc::crypto
