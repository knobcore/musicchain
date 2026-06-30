#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mc {

// ---- Constants -------------------------------------------------------

// Block format v3 — fingerprint-anchored, audio lives OUT of band,
// and (Model 1) NO validator confirmations in the header.
// Audio bytes are stored in a content-addressed file store keyed by
// content_hash (see HttpServer::audio_dir() + verb_song_audio). The
// block records only the fingerprint, content_hash, and song metadata.
//
// v3 vs v2: the BlockHeader.confirmations vote vector was removed. Under
// vote-free deterministic consensus a block is canonical because every
// node independently re-derives its validity from content + history;
// there are no stored block-level votes to tally. Transactions keep
// their own signatures — only block-level confirmation was removed.
// This makes block.hash() a single canonical hash (no separate
// signing_hash) and is a consensus-breaking format change, hence the
// version bump.
//
// Heartbeat blocks (no fingerprint submissions in 5 minutes) ship with
// has_song == 0; the SongSection is empty and only the tx pool is
// carried.
static constexpr uint32_t BLOCK_VERSION      = 3;
static constexpr uint8_t  SEPARATOR_BYTE     = 0xFF;
static constexpr size_t   SEPARATOR_LENGTH   = 8;

// Maximum block size now that audio is off-chain — ~2 MiB easily covers
// a full Dejavu constellation (~400 KB) plus thousands of transactions.
static constexpr uint64_t MAX_BLOCK_SIZE     = 2097152;   // 2 MiB

// Chain identifier mixed into every signed transaction (EIP-155 style).
// Picked so it can't collide with any existing well-known EVM chain id:
// 0x4D43 ("MC" in ASCII) → 19779. Bumped by 1 on any consensus-breaking
// rollback. A bopwire signature is NEVER replayable on Ethereum,
// BSC, Base, etc. because their chain_id contributes to the sign-hash
// preimage and ours doesn't match any of theirs.
static constexpr uint32_t MC_CHAIN_ID        = 19779;

// ---- Basic types -----------------------------------------------------

using Hash256  = std::array<uint8_t, 32>;
using Address  = std::array<uint8_t, 20>;
using PubKey33 = std::array<uint8_t, 33>;
using Sig64    = std::array<uint8_t, 64>;

// ---- Royalty split --------------------------------------------------

struct RoyaltySplit {
    Address  address;
    uint16_t basis_points; // 1-10000, sum must equal 10000
};

// ---- Audio format tag -----------------------------------------------

enum class AudioFormat : uint8_t {
    OGG     = 0x00,  // Ogg/Vorbis or Ogg/Opus
    MP3     = 0x01,
    FLAC    = 0x02,  // Lossless
    M4A     = 0x03,  // AAC or ALAC in an MP4 container
    AAC     = 0x04,  // Raw AAC ADTS stream
    OPUS    = 0x05,  // Opus in its own container
    WAV     = 0x06,  // RIFF/Wave (PCM)
    AIFF    = 0x07,  // Apple AIFF (PCM)
    WMA     = 0x08,  // Windows Media Audio
    APE     = 0x09,  // Monkey's Audio
    MKA     = 0x0A,  // Matroska Audio
    UNKNOWN = 0xFF,  // Decoder figured it out but we don't have a tag
};

// Lowercase tag string for the wire (fingerprint.submit, songs.list /
// peer entries, etc.). Pair with audio_format_from_string at the
// receiving end. Centralised so adding a format is a single-site change
// rather than a grep across the codebase.
inline const char* audio_format_to_string(AudioFormat f) {
    switch (f) {
        case AudioFormat::OGG:  return "ogg";
        case AudioFormat::MP3:  return "mp3";
        case AudioFormat::FLAC: return "flac";
        case AudioFormat::M4A:  return "m4a";
        case AudioFormat::AAC:  return "aac";
        case AudioFormat::OPUS: return "opus";
        case AudioFormat::WAV:  return "wav";
        case AudioFormat::AIFF: return "aiff";
        case AudioFormat::WMA:  return "wma";
        case AudioFormat::APE:  return "ape";
        case AudioFormat::MKA:  return "mka";
        default:                return "unknown";
    }
}

inline AudioFormat audio_format_from_string(const std::string& s) {
    if (s == "mp3")            return AudioFormat::MP3;
    if (s == "ogg")            return AudioFormat::OGG;
    if (s == "flac")           return AudioFormat::FLAC;
    if (s == "m4a")            return AudioFormat::M4A;
    if (s == "aac")            return AudioFormat::AAC;
    if (s == "opus")           return AudioFormat::OPUS;
    if (s == "wav")            return AudioFormat::WAV;
    if (s == "aif" || s == "aiff")  return AudioFormat::AIFF;
    if (s == "wma")            return AudioFormat::WMA;
    if (s == "ape")            return AudioFormat::APE;
    if (s == "mka")            return AudioFormat::MKA;
    return AudioFormat::OGG;   // backward-compat default for empty tags
}

// ---- Song section ---------------------------------------------------
//
// Carries every field the network exposes for a registered track. The
// audio bytes themselves DO NOT live here anymore — only the content
// hash + fingerprint. Audio is fetched separately from the full node's
// content-addressed file store, or from a swarm peer that announced
// itself by submitting a matching fingerprint.
struct SongSection {
    AudioFormat              audio_format = AudioFormat::OGG; // 1 byte
    Hash256                  content_hash;
    std::string              compressed_fingerprint; // base64
    uint32_t                 duration_ms;
    std::string              title;
    std::string              artist;
    Address                  artist_address;
    std::string              genre;
    std::string              album;
    // Optional ID3-style fields. Zero == unknown / not present in the
    // submitter's tags. Kept compact (4 bytes total) so they don't bloat
    // every block.
    uint16_t                 year         = 0;
    uint16_t                 track_number = 0;
    std::vector<RoyaltySplit> royalty_splits;
};

// ---- Confirmation ---------------------------------------------------
//
// (validator_id, pubkey, signature) triple. As of block format v3 this
// is NO LONGER part of the block header — Model 1 consensus stores no
// block-level votes. The struct is retained for transaction-level use:
// SlashTx EquivocationProof evidence carries Confirmations, and the type
// is available for any future per-transaction confirmation scheme. It is
// not written into BlockHeader.
struct Confirmation {
    Hash256  validator_id;
    PubKey33 pubkey;
    Sig64    signature;
};

// ---- Block header ---------------------------------------------------

struct BlockHeader {
    // Every Hash256 below is value-initialized so a freshly-default-
    // constructed BlockHeader carries all-zero hashes. The producer
    // overwrites the fields it needs (prev_hash, merkle_root, plus
    // fingerprint_hash + content_hash for song-bearing blocks) and
    // leaves the rest at zero — which is exactly what Block::validate
    // expects for heartbeat blocks. Without the {} initializers these
    // arrays would hold indeterminate values and validate() rejected
    // every block by accident.
    uint32_t                   version          = 0;
    Hash256                    prev_hash{};
    Hash256                    merkle_root{};
    Hash256                    fingerprint_hash{}; // zero on heartbeat
    Hash256                    content_hash{};     // zero on heartbeat
    uint64_t                   timestamp_ms     = 0;
    // Model 1 (vote-free deterministic consensus): the header carries NO
    // validator confirmations. Canonicality is re-derived by every node
    // from content + history (fingerprint_hash, merkle_root, prev_hash,
    // tx signatures); there are no stored block-level votes. The old
    // `confirmations` vector and the separate signing_hash() it required
    // were removed in format v3.

    // Serialise header to bytes (used for hash calculation)
    std::vector<uint8_t> serialize() const;
    // Compute block hash = SHA256(header bytes). The single chain-
    // canonical identifier — the leveldb key and what the inv/getdata
    // protocol references. With confirmations gone, this is also what a
    // producer/verifier hashes for tx-signature anchoring; there is no
    // separate signing_hash anymore.
    Hash256 hash() const;
};

// ---- Full block -----------------------------------------------------

struct Block {
    BlockHeader              header;
    bool                     has_song = false;
    SongSection              song; // valid only when has_song == true
    std::vector<std::vector<uint8_t>> transactions; // raw transaction bytes

    // Serialize entire block to bytes
    std::vector<uint8_t> serialize() const;

    // Parse block from bytes; returns false on malformed input
    static bool deserialize(const uint8_t* data, size_t len, Block& out);

    // Convenience: block hash (header, used as the canonical id). Under
    // Model 1 this is the only block hash — confirmations are not stored,
    // so there is no separate signing_hash.
    Hash256 hash()         const { return header.hash(); }

    // Compute merkle root over transactions
    static Hash256 compute_merkle_root(const std::vector<std::vector<uint8_t>>& txs);

    // Validate internal consistency (hashes, fingerprint/content match)
    bool validate() const;

    // SHA256 of the entire serialized block bytes (used for peer checksum verification)
    static Hash256 full_hash(const std::vector<uint8_t>& serialized);
};

// ---- Serialization helpers ------------------------------------------

void write_u16le(std::vector<uint8_t>& buf, uint16_t v);
void write_u32le(std::vector<uint8_t>& buf, uint32_t v);
void write_u64le(std::vector<uint8_t>& buf, uint64_t v);
void write_bytes(std::vector<uint8_t>& buf, const uint8_t* data, size_t len);
void write_string16(std::vector<uint8_t>& buf, const std::string& s);

bool read_u16le(const uint8_t*& p, const uint8_t* end, uint16_t& v);
bool read_u32le(const uint8_t*& p, const uint8_t* end, uint32_t& v);
bool read_u64le(const uint8_t*& p, const uint8_t* end, uint64_t& v);
bool read_bytes(const uint8_t*& p, const uint8_t* end, uint8_t* dst, size_t len);
bool read_string16(const uint8_t*& p, const uint8_t* end, std::string& s);

} // namespace mc
