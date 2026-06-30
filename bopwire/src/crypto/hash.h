#pragma once
#include <cstdint>
#include "../core/block.h"
#include <string>

namespace mc::crypto {

// SHA-256 of arbitrary data
Hash256 sha256(const uint8_t* data, size_t len);
Hash256 sha256(const std::vector<uint8_t>& data);
Hash256 sha256(const std::string& data);

// Double SHA-256
Hash256 sha256d(const uint8_t* data, size_t len);

// Hex encoding
std::string to_hex(const uint8_t* data, size_t len);
std::string to_hex(const Hash256& h);
std::string to_hex(const std::vector<uint8_t>& v);

// Hex decoding; returns empty vector on malformed input
std::vector<uint8_t> from_hex(const std::string& hex);

// Parse a 32-byte hex string into Hash256; returns false on failure
bool parse_hash256(const std::string& hex, Hash256& out);

// Parse a 20-byte hex string into Address; returns false on failure
bool parse_address(const std::string& hex, Address& out);

// Derive an "escrow" address from an artist's address. The result is
// deterministic, has no corresponding private key (so no one can sign
// a transfer FROM it), and acts as a per-artist holding account that
// the moderator can release via the existing transfer-by-moderator
// endpoint. Used by the mint logic: while the song's play_count is
// under 10000, the artist's share is credited here instead of the
// artist's spendable balance, then released by the moderator on
// approval.
Address escrow_address_for(const Address& artist);

} // namespace mc::crypto
