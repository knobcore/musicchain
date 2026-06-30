#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mc::audio {

// Shared chromaprint same-song similarity threshold. A pair scoring >= this
// under Fingerprint::similarity() is treated as the SAME song (swarm-join, no
// duplicate block). This was hardcoded as 0.55 in TWO call sites — chain.cpp's
// replay dup-check (consensus) and rats_api.cpp's swarm-join (API). They MUST
// agree or nodes disagree on duplicate-ness and fork, so it lives here now and
// both reference it. Raised 0.55 -> 0.70 together with the similarity()
// offset-alignment fix + minimum-overlap guard: with those, different songs
// collapse toward ~0 and same-song re-encodes sit in the high band, so 0.70
// separates them cleanly (0.55 sat inside the inflated different-song band).
inline constexpr float kChromaprintSimThreshold = 0.70f;

// Chromaprint-based audio fingerprinting
class Fingerprint {
public:
    // Generate fingerprint from raw Ogg data (decodes audio internally)
    static std::unique_ptr<Fingerprint> from_ogg(const uint8_t* data, size_t len);

    // Generate fingerprint from any container/codec FFmpeg can read
    // (MP3 / FLAC / WAV / AAC / Opus / …). Used by DeepAuditor so the
    // chromaprint↔audio gate isn't restricted to Ogg-format blocks.
    static std::unique_ptr<Fingerprint> from_any(const uint8_t* data,
                                                  size_t len);

    // Load from stored base64-compressed string
    static std::unique_ptr<Fingerprint> from_compressed(const std::string& base64);

    // Return base64-compressed fingerprint (for storage)
    std::string compressed() const;

    // Raw uint32 array
    const std::vector<uint32_t>& raw() const { return raw_; }

    // Compute similarity [0.0, 1.0] between this and another fingerprint
    float similarity(const Fingerprint& other) const;

    // Compute bucket IDs for inverted index (up to 100)
    std::vector<uint16_t> bucket_ids() const;

private:
    std::vector<uint32_t> raw_;
};

// Base64 encode / decode helpers
std::string base64_encode(const uint8_t* data, size_t len);
std::vector<uint8_t> base64_decode(const std::string& s);

} // namespace mc::audio
