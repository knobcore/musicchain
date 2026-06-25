#include "block.h"
#include "../crypto/hash.h"
#include <cstring>
#include <stdexcept>

namespace mc {

// ---- Serialization helpers ------------------------------------------

void write_u16le(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void write_u32le(std::vector<uint8_t>& buf, uint32_t v) {
    for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>((v >> (8*i)) & 0xFF));
}

void write_u64le(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 0; i < 8; ++i) buf.push_back(static_cast<uint8_t>((v >> (8*i)) & 0xFF));
}

void write_bytes(std::vector<uint8_t>& buf, const uint8_t* data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

void write_string16(std::vector<uint8_t>& buf, const std::string& s) {
    write_u16le(buf, static_cast<uint16_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
}

bool read_u16le(const uint8_t*& p, const uint8_t* end, uint16_t& v) {
    if (end - p < 2) return false;
    v = static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    p += 2; return true;
}

bool read_u32le(const uint8_t*& p, const uint8_t* end, uint32_t& v) {
    if (end - p < 4) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) v |= (static_cast<uint32_t>(p[i]) << (8*i));
    p += 4; return true;
}

bool read_u64le(const uint8_t*& p, const uint8_t* end, uint64_t& v) {
    if (end - p < 8) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) v |= (static_cast<uint64_t>(p[i]) << (8*i));
    p += 8; return true;
}

bool read_bytes(const uint8_t*& p, const uint8_t* end, uint8_t* dst, size_t len) {
    if (static_cast<size_t>(end - p) < len) return false;
    std::memcpy(dst, p, len);
    p += len; return true;
}

bool read_string16(const uint8_t*& p, const uint8_t* end, std::string& s) {
    uint16_t len = 0;
    if (!read_u16le(p, end, len)) return false;
    if (static_cast<size_t>(end - p) < len) return false;
    s.assign(reinterpret_cast<const char*>(p), len);
    p += len; return true;
}

// ---- BlockHeader ----------------------------------------------------

std::vector<uint8_t> BlockHeader::serialize() const {
    std::vector<uint8_t> buf;
    buf.reserve(256);
    write_u32le(buf, version);
    write_bytes(buf, prev_hash.data(),        32);
    write_bytes(buf, merkle_root.data(),      32);
    write_bytes(buf, fingerprint_hash.data(), 32);
    write_bytes(buf, content_hash.data(),     32);
    write_u64le(buf, timestamp_ms);
    // Model 1 / format v3: no confirmations vector in the header.
    return buf;
}

Hash256 BlockHeader::hash() const {
    auto hdr = serialize();
    return crypto::sha256(hdr.data(), hdr.size());
}

// ---- Block serialization --------------------------------------------

std::vector<uint8_t> Block::serialize() const {
    std::vector<uint8_t> buf;

    // Header
    auto hdr = header.serialize();
    buf.insert(buf.end(), hdr.begin(), hdr.end());

    // Optional song record
    buf.push_back(has_song ? 0x01 : 0x00);
    if (has_song) {
        buf.push_back(static_cast<uint8_t>(song.audio_format));
        write_bytes(buf, song.content_hash.data(), 32);
        write_string16(buf, song.compressed_fingerprint);
        write_u32le(buf, song.duration_ms);
        write_string16(buf, song.title);
        write_string16(buf, song.artist);
        write_bytes(buf, song.artist_address.data(), 20);
        write_string16(buf, song.genre);
        write_string16(buf, song.album);
        // ID3-style optional fields — 0 means "not provided".
        write_u16le(buf, song.year);
        write_u16le(buf, song.track_number);
        buf.push_back(static_cast<uint8_t>(song.royalty_splits.size()));
        for (const auto& rs : song.royalty_splits) {
            write_bytes(buf, rs.address.data(), 20);
            write_u16le(buf, rs.basis_points);
        }
    }

    // Separator
    for (size_t i = 0; i < SEPARATOR_LENGTH; ++i) buf.push_back(SEPARATOR_BYTE);

    // Transaction section
    write_u32le(buf, static_cast<uint32_t>(transactions.size()));
    for (const auto& tx : transactions) {
        write_u32le(buf, static_cast<uint32_t>(tx.size()));
        buf.insert(buf.end(), tx.begin(), tx.end());
    }

    return buf;
}

bool Block::deserialize(const uint8_t* data, size_t len, Block& out) {
    const uint8_t* p   = data;
    const uint8_t* end = data + len;

    // --- Header ---
    if (!read_u32le(p, end, out.header.version)) return false;
    if (!read_bytes(p, end, out.header.prev_hash.data(),        32)) return false;
    if (!read_bytes(p, end, out.header.merkle_root.data(),      32)) return false;
    if (!read_bytes(p, end, out.header.fingerprint_hash.data(), 32)) return false;
    if (!read_bytes(p, end, out.header.content_hash.data(),     32)) return false;
    if (!read_u64le(p, end, out.header.timestamp_ms))               return false;
    // Model 1 / format v3: no confirmations vector to read.

    // --- Optional song record ---
    if (p >= end) return false;
    out.has_song = (*p++ != 0);
    if (out.has_song) {
        if (p >= end) return false;
        out.song.audio_format = static_cast<AudioFormat>(*p++);
        if (!read_bytes(p, end, out.song.content_hash.data(), 32)) return false;
        if (!read_string16(p, end, out.song.compressed_fingerprint)) return false;
        if (!read_u32le(p, end, out.song.duration_ms))               return false;
        if (!read_string16(p, end, out.song.title))                  return false;
        if (!read_string16(p, end, out.song.artist))                 return false;
        if (!read_bytes(p, end, out.song.artist_address.data(), 20)) return false;
        if (!read_string16(p, end, out.song.genre))                  return false;
        if (!read_string16(p, end, out.song.album))                  return false;
        if (!read_u16le(p, end, out.song.year))                      return false;
        if (!read_u16le(p, end, out.song.track_number))              return false;
        if (p >= end) return false;
        uint8_t rs_count = *p++;
        out.song.royalty_splits.resize(rs_count);
        for (auto& rs : out.song.royalty_splits) {
            if (!read_bytes(p, end, rs.address.data(), 20)) return false;
            if (!read_u16le(p, end, rs.basis_points))      return false;
        }
    }

    // --- Separator ---
    if (static_cast<size_t>(end - p) < SEPARATOR_LENGTH) return false;
    for (size_t i = 0; i < SEPARATOR_LENGTH; ++i) {
        if (p[i] != SEPARATOR_BYTE) return false;
    }
    p += SEPARATOR_LENGTH;

    // --- Transaction section ---
    uint32_t tx_count = 0;
    if (!read_u32le(p, end, tx_count)) return false;
    out.transactions.resize(tx_count);
    for (auto& tx : out.transactions) {
        uint32_t tx_len = 0;
        if (!read_u32le(p, end, tx_len)) return false;
        if (static_cast<size_t>(end - p) < tx_len) return false;
        tx.assign(p, p + tx_len);
        p += tx_len;
    }

    return true;
}

Hash256 Block::compute_merkle_root(const std::vector<std::vector<uint8_t>>& txs) {
    if (txs.empty()) {
        Hash256 zero{};
        return zero;
    }
    std::vector<Hash256> hashes;
    hashes.reserve(txs.size());
    for (const auto& tx : txs)
        hashes.push_back(crypto::sha256(tx.data(), tx.size()));

    while (hashes.size() > 1) {
        if (hashes.size() % 2 != 0)
            hashes.push_back(hashes.back());
        std::vector<Hash256> next;
        next.reserve(hashes.size() / 2);
        for (size_t i = 0; i < hashes.size(); i += 2) {
            std::vector<uint8_t> combined;
            combined.insert(combined.end(), hashes[i].begin(), hashes[i].end());
            combined.insert(combined.end(), hashes[i+1].begin(), hashes[i+1].end());
            next.push_back(crypto::sha256(combined.data(), combined.size()));
        }
        hashes = std::move(next);
    }
    return hashes[0];
}

Hash256 Block::full_hash(const std::vector<uint8_t>& serialized) {
    return crypto::sha256(serialized.data(), serialized.size());
}

bool Block::validate() const {
    // Heartbeat blocks: no song record, header.fingerprint_hash and
    // header.content_hash must both be zero.
    if (!has_song) {
        Hash256 zero{};
        if (header.fingerprint_hash != zero) return false;
        if (header.content_hash     != zero) return false;
    } else {
        // Song blocks: header content_hash must match the body's
        // content_hash and the body fingerprint must hash to the header's
        // fingerprint_hash. Audio bytes themselves are not in the block
        // (off-chain content-addressed store), so we don't verify them
        // here — the consumer that fetches them by content_hash will.
        if (header.content_hash != song.content_hash) return false;
        const auto& fp = song.compressed_fingerprint;
        Hash256 fph = crypto::sha256(
            reinterpret_cast<const uint8_t*>(fp.data()), fp.size());
        if (header.fingerprint_hash != fph) return false;
    }
    // Verify merkle_root
    if (compute_merkle_root(transactions) != header.merkle_root)
        return false;
    return true;
}

} // namespace mc
