#pragma once
#include <array>
#include <cstdint>
#include <cstddef>

namespace mc::crypto {

// Ethereum-style Keccak-256 (NB: NOT the same as NIST SHA-3-256).
//
// We need this because MusicChain addresses are EVM-compatible: the
// 20-byte address is keccak256(uncompressed_pubkey)[12..32], exactly as
// Ethereum/MetaMask derive it, so the same secp256k1 key yields the same
// address in any standard EVM wallet. OpenSSL only exposes the SHA-3
// variant (which uses a different padding byte — `0x06` instead of
// Keccak's `0x01` — so the digests diverge for any input), hence this
// self-contained implementation.
// (The former Base/mcCOIN bridge that originally motivated this was
// removed — keccak is retained purely for EVM-address compatibility.)
//
// Self-contained reference implementation of the Keccak-f[1600]
// permutation. ~200 lines, no external deps. The constants are
// directly from the FIPS-202 / Keccak spec.
//
// Domain check: feeding the empty string to this function yields
//   c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470
// which is the canonical "keccak256("")" value referenced in every
// Ethereum implementation.

std::array<uint8_t, 32> keccak256(const uint8_t* data, size_t len);

} // namespace mc::crypto
