#include "ogg_decoder.h"
#include "ogg_validator.h"
#include <vorbis/vorbisfile.h>
#include <opus/opusfile.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}
#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace mc::audio {

// ---- Memory datasource callbacks for ov_open_callbacks ---------------

struct MemBuf {
    const uint8_t* data;
    size_t         len;
    size_t         pos = 0;
};

static size_t mem_read(void* ptr, size_t size, size_t nmemb, void* ds) {
    auto* m = static_cast<MemBuf*>(ds);
    size_t available = m->len - m->pos;
    size_t to_read   = std::min(size * nmemb, available);
    std::memcpy(ptr, m->data + m->pos, to_read);
    m->pos += to_read;
    return to_read / size;
}

static int mem_seek(void* ds, ogg_int64_t offset, int whence) {
    auto* m = static_cast<MemBuf*>(ds);
    ogg_int64_t new_pos = 0;
    if (whence == SEEK_SET)      new_pos = offset;
    else if (whence == SEEK_CUR) new_pos = static_cast<ogg_int64_t>(m->pos) + offset;
    else if (whence == SEEK_END) new_pos = static_cast<ogg_int64_t>(m->len) + offset;
    else return -1;
    if (new_pos < 0 || static_cast<size_t>(new_pos) > m->len) return -1;
    m->pos = static_cast<size_t>(new_pos);
    return 0;
}

static long mem_tell(void* ds) {
    return static_cast<long>(static_cast<MemBuf*>(ds)->pos);
}

static int mem_close(void*) { return 0; }

// ---- Vorbis decoder -------------------------------------------------

class VorbisDecoder : public OggDecoder {
public:
    VorbisDecoder(const uint8_t* data, size_t len) {
        mem_ = {data, len};
        ov_callbacks cbs{mem_read, mem_seek, mem_close, mem_tell};
        if (ov_open_callbacks(&mem_, &vf_, nullptr, 0, cbs) < 0)
            throw std::runtime_error("ov_open_callbacks failed");
        vorbis_info* info = ov_info(&vf_, -1);
        sample_rate_ = static_cast<int>(info->rate);
        channels_    = info->channels;
        ogg_int64_t total = ov_pcm_total(&vf_, -1);
        duration_ms_ = (total > 0) ? static_cast<uint32_t>((total * 1000LL) / sample_rate_) : 0;
    }

    ~VorbisDecoder() override { ov_clear(&vf_); }

    int      sample_rate()  const override { return sample_rate_; }
    int      channels()     const override { return channels_; }
    uint32_t duration_ms()  const override { return duration_ms_; }

    int read(int16_t* buf, int max_samples) override {
        int bytes_to_read = max_samples * static_cast<int>(sizeof(int16_t));
        int bitstream = 0;
        long ret = ov_read(&vf_, reinterpret_cast<char*>(buf), bytes_to_read,
                           0, 2, 1, &bitstream);
        if (ret == 0) return 0;
        if (ret < 0)  return -1;
        return static_cast<int>(ret / sizeof(int16_t));
    }

    bool seek(uint32_t position_ms) override {
        return ov_time_seek(&vf_, position_ms / 1000.0) == 0;
    }

    uint32_t position_ms() const override {
        double t = ov_time_tell(const_cast<OggVorbis_File*>(&vf_));
        return static_cast<uint32_t>(t * 1000.0);
    }

private:
    MemBuf         mem_;
    OggVorbis_File vf_{};
    int            sample_rate_ = 0;
    int            channels_    = 0;
    uint32_t       duration_ms_ = 0;
};

// ---- Opus decoder ---------------------------------------------------

class OpusDecoder_ : public OggDecoder {
public:
    OpusDecoder_(const uint8_t* data, size_t len) {
        int err = 0;
        of_ = op_open_memory(data, len, &err);
        if (!of_) throw std::runtime_error("op_open_memory failed");
        channels_    = op_channel_count(of_, -1);
        ogg_int64_t total = op_pcm_total(of_, -1);
        duration_ms_ = (total > 0) ? static_cast<uint32_t>((total * 1000LL) / 48000) : 0;
    }

    ~OpusDecoder_() override { op_free(of_); }

    int      sample_rate()  const override { return 48000; }
    int      channels()     const override { return channels_; }
    uint32_t duration_ms()  const override { return duration_ms_; }

    int read(int16_t* buf, int max_samples) override {
        int samples_per_ch = max_samples / channels_;
        int ret = op_read(of_, buf, samples_per_ch, nullptr);
        if (ret < 0) return -1;
        return ret * channels_;
    }

    bool seek(uint32_t position_ms) override {
        ogg_int64_t sample = static_cast<ogg_int64_t>(position_ms) * 48 / 1000;
        return op_pcm_seek(of_, sample) == 0;
    }

    uint32_t position_ms() const override {
        ogg_int64_t pos = op_pcm_tell(of_);
        return static_cast<uint32_t>((pos * 1000LL) / 48000);
    }

private:
    OggOpusFile* of_          = nullptr;
    int          channels_    = 0;
    uint32_t     duration_ms_ = 0;
};

// ---- FFmpeg-based MP3 decoder --------------------------------------

struct FfmpegMemIO {
    std::vector<uint8_t> data; // owns a copy
    size_t               pos = 0;
};

static int ffmpeg_dec_read(void* opaque, uint8_t* buf, int size) {
    auto* m = static_cast<FfmpegMemIO*>(opaque);
    if (m->pos >= m->data.size()) return AVERROR_EOF;
    int to_read = static_cast<int>(
        std::min<size_t>(size, m->data.size() - m->pos));
    std::memcpy(buf, m->data.data() + m->pos, to_read);
    m->pos += to_read;
    return to_read;
}

static int64_t ffmpeg_dec_seek(void* opaque, int64_t offset, int whence) {
    auto* m = static_cast<FfmpegMemIO*>(opaque);
    int64_t new_pos = 0;
    if (whence == SEEK_SET)         new_pos = offset;
    else if (whence == SEEK_CUR)    new_pos = static_cast<int64_t>(m->pos) + offset;
    else if (whence == SEEK_END)    new_pos = static_cast<int64_t>(m->data.size()) + offset;
    else if (whence == AVSEEK_SIZE) return static_cast<int64_t>(m->data.size());
    else return -1;
    if (new_pos < 0 || static_cast<size_t>(new_pos) > m->data.size()) return -1;
    m->pos = static_cast<size_t>(new_pos);
    return new_pos;
}

class FfmpegDecoder : public OggDecoder {
public:
    FfmpegDecoder(const uint8_t* data, size_t len) {
        // Copy data so the decoder is self-contained
        mem_.data.assign(data, data + len);

        constexpr int IO_BUF = 65536;
        uint8_t* io_buf = static_cast<uint8_t*>(av_malloc(IO_BUF));
        avio_ = avio_alloc_context(io_buf, IO_BUF, 0, &mem_,
                                   ffmpeg_dec_read, nullptr, ffmpeg_dec_seek);
        if (!avio_) throw std::runtime_error("avio_alloc_context failed");

        fmt_ = avformat_alloc_context();
        fmt_->pb = avio_;

        if (avformat_open_input(&fmt_, nullptr, nullptr, nullptr) < 0)
            throw std::runtime_error("avformat_open_input failed");
        if (avformat_find_stream_info(fmt_, nullptr) < 0)
            throw std::runtime_error("avformat_find_stream_info failed");

        int idx = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (idx < 0) throw std::runtime_error("no audio stream in MP3");
        stream_idx_ = idx;

        AVStream* stream = fmt_->streams[idx];
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) throw std::runtime_error("codec not found");

        codec_ctx_ = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(codec_ctx_, stream->codecpar);
        if (avcodec_open2(codec_ctx_, codec, nullptr) < 0)
            throw std::runtime_error("avcodec_open2 failed");

        sample_rate_ = codec_ctx_->sample_rate;
        // FFmpeg 5.1 (libavcodec 59) introduced the AVChannelLayout
        // struct (codec_ctx_->ch_layout). Earlier releases — including
        // Ubuntu 22.04's libavcodec 58 — only expose the legacy
        // channel_layout / channels fields. Pick whichever this build
        // has at compile time.
#if LIBAVCODEC_VERSION_MAJOR >= 59
        channels_    = codec_ctx_->ch_layout.nb_channels;
#else
        channels_    = codec_ctx_->channels;
#endif

        // Duration
        if (fmt_->duration > 0) {
            duration_ms_ = static_cast<uint32_t>(fmt_->duration / 1000);
        } else if (stream->duration > 0 && stream->time_base.den > 0) {
            double secs = static_cast<double>(stream->duration) *
                          stream->time_base.num / stream->time_base.den;
            duration_ms_ = static_cast<uint32_t>(secs * 1000.0);
        }

        // Set up SwrContext to convert to s16 interleaved
        swr_ = swr_alloc();
#if LIBAVCODEC_VERSION_MAJOR >= 59
        av_opt_set_chlayout(swr_,  "in_chlayout",   &codec_ctx_->ch_layout,  0);
        av_opt_set_chlayout(swr_,  "out_chlayout",  &codec_ctx_->ch_layout,  0);
#else
        // Fall back to the integer layout mask + av_opt_set_channel_layout
        // signature that FFmpeg 4.x exposes.
        int64_t layout = static_cast<int64_t>(codec_ctx_->channel_layout);
        if (layout == 0) {
            // Streams sometimes omit channel_layout; reconstruct from the
            // channel count so swresample doesn't bail.
            layout = static_cast<int64_t>(
                av_get_default_channel_layout(codec_ctx_->channels));
        }
        av_opt_set_channel_layout(swr_, "in_channel_layout",  layout, 0);
        av_opt_set_channel_layout(swr_, "out_channel_layout", layout, 0);
#endif
        av_opt_set_int(swr_, "in_sample_rate",  sample_rate_, 0);
        av_opt_set_int(swr_, "out_sample_rate", sample_rate_, 0);
        av_opt_set_sample_fmt(swr_, "in_sample_fmt",  codec_ctx_->sample_fmt, 0);
        av_opt_set_sample_fmt(swr_, "out_sample_fmt", AV_SAMPLE_FMT_S16,      0);
        swr_init(swr_);

        frame_  = av_frame_alloc();
        packet_ = av_packet_alloc();
    }

    ~FfmpegDecoder() override {
        av_packet_free(&packet_);
        av_frame_free(&frame_);
        swr_free(&swr_);
        avcodec_free_context(&codec_ctx_);
        avformat_close_input(&fmt_);
        if (avio_) {
            av_free(avio_->buffer);
            avio_context_free(&avio_);
        }
    }

    int      sample_rate()  const override { return sample_rate_; }
    int      channels()     const override { return channels_; }
    uint32_t duration_ms()  const override { return duration_ms_; }

    int read(int16_t* out_buf, int max_samples) override {
        int written = 0;

        // Drain any leftover converted samples first
        while (written < max_samples && !pcm_buf_.empty()) {
            out_buf[written++] = pcm_buf_.front();
            pcm_buf_.erase(pcm_buf_.begin());
        }

        while (written < max_samples) {
            // Get a decoded frame
            int ret = avcodec_receive_frame(codec_ctx_, frame_);
            if (ret == AVERROR(EAGAIN)) {
                // Send another packet
                ret = av_read_frame(fmt_, packet_);
                if (ret == AVERROR_EOF) {
                    // Flush decoder
                    avcodec_send_packet(codec_ctx_, nullptr);
                    ret = avcodec_receive_frame(codec_ctx_, frame_);
                    if (ret < 0) return written == 0 ? 0 : written;
                } else if (ret < 0) {
                    return written == 0 ? -1 : written;
                } else {
                    if (packet_->stream_index == stream_idx_) {
                        avcodec_send_packet(codec_ctx_, packet_);
                    }
                    av_packet_unref(packet_);
                    continue;
                }
            } else if (ret < 0) {
                return written == 0 ? -1 : written;
            }

            // Convert frame to s16 interleaved
            int n_samples = frame_->nb_samples;
            std::vector<int16_t> tmp(n_samples * channels_);
            uint8_t* out_ptr = reinterpret_cast<uint8_t*>(tmp.data());
            swr_convert(swr_, &out_ptr, n_samples,
                        const_cast<const uint8_t**>(frame_->data), n_samples);

            // Track position
            if (frame_->pts != AV_NOPTS_VALUE) {
                AVStream* s = fmt_->streams[stream_idx_];
                double secs = static_cast<double>(frame_->pts) *
                              s->time_base.num / s->time_base.den;
                position_ms_ = static_cast<uint32_t>(secs * 1000.0);
            }

            av_frame_unref(frame_);

            // Copy to output, stash overflow
            for (int i = 0; i < static_cast<int>(tmp.size()); ++i) {
                if (written < max_samples) {
                    out_buf[written++] = tmp[i];
                } else {
                    pcm_buf_.push_back(tmp[i]);
                }
            }
        }
        return written;
    }

    bool seek(uint32_t pos_ms) override {
        int64_t ts = static_cast<int64_t>(pos_ms) * AV_TIME_BASE / 1000;
        bool ok = av_seek_frame(fmt_, -1, ts, AVSEEK_FLAG_BACKWARD) >= 0;
        avcodec_flush_buffers(codec_ctx_);
        pcm_buf_.clear();
        position_ms_ = pos_ms;
        return ok;
    }

    uint32_t position_ms() const override { return position_ms_; }

private:
    FfmpegMemIO       mem_;
    AVIOContext*      avio_       = nullptr;
    AVFormatContext*  fmt_        = nullptr;
    AVCodecContext*   codec_ctx_  = nullptr;
    SwrContext*       swr_        = nullptr;
    AVFrame*          frame_      = nullptr;
    AVPacket*         packet_     = nullptr;
    int               stream_idx_ = -1;
    int               sample_rate_ = 0;
    int               channels_    = 0;
    uint32_t          duration_ms_ = 0;
    uint32_t          position_ms_ = 0;
    std::vector<int16_t> pcm_buf_; // overflow from last decoded frame
};

// ---- Factory --------------------------------------------------------

std::unique_ptr<OggDecoder> OggDecoder::open(const uint8_t* data, size_t len) {
    if (len < 4) throw std::runtime_error("audio data too short");

    if (is_ogg_magic(data, len)) {
        // Distinguish Opus vs Vorbis by scanning early bytes
        bool is_opus = false;
        for (size_t i = 4; i + 8 <= std::min(len, size_t(256)); ++i) {
            if (std::memcmp(data + i, "OpusHead", 8) == 0) { is_opus = true; break; }
        }
        if (is_opus) return std::make_unique<OpusDecoder_>(data, len);
        return std::make_unique<VorbisDecoder>(data, len);
    }

    if (is_mp3_magic(data, len)) {
        return std::make_unique<FfmpegDecoder>(data, len);
    }

    throw std::runtime_error("unrecognized audio format (expected Ogg or MP3)");
}

// ---- ChecksumBuffer -------------------------------------------------

ChecksumBuffer::ChecksumBuffer(int sample_rate, int channels)
    : sample_rate_(sample_rate), channels_(channels) {
    capacity_ = static_cast<size_t>(sample_rate) * 5 * channels;
    buf_.resize(capacity_, 0);
}

void ChecksumBuffer::feed(const int16_t* samples, int count) {
    for (int i = 0; i < count; ++i) {
        buf_[head_] = samples[i];
        head_ = (head_ + 1) % capacity_;
        if (filled_ < capacity_) ++filled_;
    }
}

uint32_t ChecksumBuffer::compute() const {
    uint64_t acc   = 0;
    size_t   n     = std::min(filled_, capacity_);
    size_t   start = (head_ + capacity_ - n) % capacity_;
    for (size_t i = 0; i < n; ++i) {
        int16_t s = buf_[(start + i) % capacity_];
        acc += static_cast<uint64_t>(s < 0 ? -s : s);
    }
    return static_cast<uint32_t>(acc & 0xFFFFFFFFULL);
}

void ChecksumBuffer::reset() {
    std::fill(buf_.begin(), buf_.end(), int16_t(0));
    head_   = 0;
    filled_ = 0;
}

} // namespace mc::audio
