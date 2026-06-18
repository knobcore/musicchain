#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mc::audio {

// In-memory audio decode covering every container/codec FFmpeg can read.
// Used by DeepAuditor so we can recompute chromaprint on MP3 / FLAC / WAV
// / AAC blocks — not just Ogg. Returns int16 interleaved PCM resampled
// to (sample_rate, channels) chosen by the decoder. Returns an empty
// vector + zero rate/channels on failure (corrupt bytes, unsupported
// codec, OOM, etc).
//
// Why not stream: chromaprint needs ~30 s of audio to make a decent
// fingerprint, and most music registrations are 2-5 MB compressed. The
// full PCM expansion is ~50 MB which we tolerate for an offline audit
// task. Streaming would complicate the API for no real win at our scale.
struct DecodedPcm {
    std::vector<int16_t> samples;       // interleaved L,R,L,R,...
    int                  sample_rate = 0;
    int                  channels    = 0;
};

DecodedPcm decode_any(const uint8_t* data, size_t len);

} // namespace mc::audio
