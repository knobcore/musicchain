#include "merkle.h"
#include "../crypto/hash.h"

namespace mc {

Hash256 merkle_root(const std::vector<Hash256>& leaves) {
    if (leaves.empty()) return Hash256{};

    std::vector<Hash256> cur = leaves;
    while (cur.size() > 1) {
        if (cur.size() % 2) cur.push_back(cur.back());
        std::vector<Hash256> next;
        next.reserve(cur.size() / 2);
        for (size_t i = 0; i < cur.size(); i += 2) {
            std::vector<uint8_t> combined;
            combined.insert(combined.end(), cur[i].begin(), cur[i].end());
            combined.insert(combined.end(), cur[i+1].begin(), cur[i+1].end());
            next.push_back(crypto::sha256(combined.data(), combined.size()));
        }
        cur = std::move(next);
    }
    return cur[0];
}

Hash256 merkle_root_bytes(const std::vector<std::vector<uint8_t>>& items) {
    std::vector<Hash256> leaves;
    leaves.reserve(items.size());
    for (const auto& item : items)
        leaves.push_back(crypto::sha256(item.data(), item.size()));
    return merkle_root(leaves);
}

} // namespace mc
