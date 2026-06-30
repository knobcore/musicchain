#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace mc::audio {

// Abstract PCM decoder for Ogg/Vorbis, Ogg/Opus, and MP3.
class OggDecoder {
public:
    virtual ~OggDecoder() = default;

    // Open a decoder from raw audio data (Ogg or MP3).
    // Data must outlive the decoder for Ogg-based decoders;
    // FFmpeg-based (MP3) decoder copies data internally.
    static std::unique_ptr<OggDecoder> open(const uint8_t* data, size_t len);

    virtual int      sample_rate()  const = 0;
    virtual int      channels()     const = 0;
    virtual uint32_t duration_ms()  const = 0;

    // Decode up to max_samples interleaved signed-16-bit samples.
    // Returns number of samples decoded, 0 at EOF, -1 on error.
    virtual int read(int16_t* buf, int max_samples) = 0;

    // Seek to position in milliseconds; returns false if unsupported
    virtual bool seek(uint32_t position_ms) = 0;

    // Current playback position in ms
    virtual uint32_t position_ms() const = 0;
};

// ---- Audio checksum ------------------------------------------------

// Circular buffer that holds the last 5 seconds of decoded PCM.
// Used to compute the heartbeat checksum.
class ChecksumBuffer {
public:
    explicit ChecksumBuffer(int sample_rate, int channels);

    // Feed decoded samples into the buffer
    void feed(const int16_t* samples, int count);

    // Compute checksum: sum of abs values of last 5 seconds, mod 2^32
    uint32_t compute() const;

    void reset();

private:
    int                  sample_rate_;
    int                  channels_;
    size_t               capacity_;  // samples (5 sec * rate * ch)
    std::vector<int16_t> buf_;
    size_t               head_ = 0;
    size_t               filled_ = 0;
};

} // namespace mc::audio
