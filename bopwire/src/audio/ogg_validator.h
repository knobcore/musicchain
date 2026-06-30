#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mc::audio {

enum class AudioCodec { VORBIS, OPUS, MP3, UNKNOWN };

// Legacy alias kept for existing callers
using OggCodec = AudioCodec;

struct AudioInfo {
    AudioCodec codec        = AudioCodec::UNKNOWN;
    int        channels     = 0;
    int        sample_rate  = 0;
    uint32_t   duration_ms  = 0;
};

// Legacy alias
using OggInfo = AudioInfo;

struct AudioValidationResult {
    bool        valid   = false;
    AudioInfo   info;
    std::string error;
};

// Legacy alias
using OggValidationResult = AudioValidationResult;

// Validate any supported audio format (Ogg/Vorbis, Ogg/Opus, MP3).
// Checks format integrity, codec parameters, and minimum duration (>= 30 s).
AudioValidationResult validate_audio(const uint8_t* data, size_t len);

// Legacy wrapper — delegates to validate_audio
inline AudioValidationResult validate_ogg(const uint8_t* data, size_t len) {
    return validate_audio(data, len);
}

// Quick magic-byte detection
bool is_ogg_magic(const uint8_t* data, size_t len);
bool is_mp3_magic(const uint8_t* data, size_t len);

} // namespace mc::audio
