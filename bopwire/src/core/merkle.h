#pragma once
#include <cstdint>
#include "block.h"
#include <vector>

namespace mc {

// Standalone Merkle utilities (delegating to Block::compute_merkle_root)
Hash256 merkle_root(const std::vector<Hash256>& leaves);
Hash256 merkle_root_bytes(const std::vector<std::vector<uint8_t>>& items);

} // namespace mc
