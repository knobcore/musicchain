#include "ogg_validator.h"
#include <ogg/ogg.h>
#include <vorbis/codec.h>
#include <opus/opusfile.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}
#include <cstring>
#include <algorithm>

namespace mc::audio {

bool is_ogg_magic(const uint8_t* data, size_t len) {
    return len >= 4 && std::memcmp(data, "OggS", 4) == 0;
}

bool is_mp3_magic(const uint8_t* data, size_t len) {
    if (len < 3) return false;
    // ID3 tag header
    if (std::memcmp(data, "ID3", 3) == 0) return true;
    // MPEG sync word (0xFF 0xE* or 0xFF 0xF*)
    if (len >= 2 && data[0] == 0xFF && (data[1] & 0xE0) == 0xE0) return true;
    return false;
}

namespace {

// ---- Ogg validation (Vorbis + Opus) ---------------------------------

AudioValidationResult validate_ogg_internal(const uint8_t* data, size_t len) {
    ogg_sync_state   oy;
    ogg_stream_state os;
    ogg_page         og;
    ogg_packet       op;

    ogg_sync_init(&oy);

    char* buffer = ogg_sync_buffer(&oy, static_cast<long>(len));
    std::memcpy(buffer, data, len);
    ogg_sync_wrote(&oy, static_cast<long>(len));

    AudioValidationResult result;

    if (ogg_sync_pageout(&oy, &og) != 1) {
        result.error = "failed to read first Ogg page";
        ogg_sync_clear(&oy);
        return result;
    }

    ogg_stream_init(&os, ogg_page_serialno(&og));
    ogg_stream_pagein(&os, &og);

    if (ogg_stream_packetout(&os, &op) != 1) {
        result.error = "failed to get first Ogg packet";
        goto cleanup;
    }

    if (op.bytes >= 7 && op.packet[0] == 0x01 &&
        std::memcmp(op.packet + 1, "vorbis", 6) == 0) {
        // Vorbis
        vorbis_info vi;
        vorbis_comment vc;
        vorbis_info_init(&vi);
        vorbis_comment_init(&vc);

        if (vorbis_synthesis_headerin(&vi, &vc, &op) < 0) {
            result.error = "invalid Vorbis identification header";
            vorbis_info_clear(&vi);
            vorbis_comment_clear(&vc);
            goto cleanup;
        }

        result.info.codec       = AudioCodec::VORBIS;
        result.info.channels    = vi.channels;
        result.info.sample_rate = static_cast<int>(vi.rate);

        int headers = 0;
        while (headers < 2) {
            if (ogg_sync_pageout(&oy, &og) == 1) {
                ogg_stream_pagein(&os, &og);
                while (ogg_stream_packetout(&os, &op) == 1) {
                    vorbis_synthesis_headerin(&vi, &vc, &op);
                    ++headers;
                }
            } else break;
        }

        ogg_int64_t last_granule = 0;
        while (ogg_sync_pageout(&oy, &og) == 1) {
            ogg_int64_t gp = ogg_page_granulepos(&og);
            if (gp > 0) last_granule = gp;
        }
        if (vi.rate > 0)
            result.info.duration_ms = static_cast<uint32_t>(
                (last_granule * 1000LL) / vi.rate);

        vorbis_info_clear(&vi);
        vorbis_comment_clear(&vc);
        result.valid = true;

    } else if (op.bytes >= 8 && std::memcmp(op.packet, "OpusHead", 8) == 0) {
        // Opus
        result.info.codec       = AudioCodec::OPUS;
        result.info.channels    = (op.bytes >= 10) ? op.packet[9] : 0;
        result.info.sample_rate = 48000;

        ogg_int64_t last_granule = 0;
        while (ogg_sync_pageout(&oy, &og) == 1) {
            ogg_int64_t gp = ogg_page_granulepos(&og);
            if (gp > 0) last_granule = gp;
        }
        result.info.duration_ms = static_cast<uint32_t>(
            (last_granule * 1000LL) / 48000);
        result.valid = true;

    } else {
        result.error = "unsupported Ogg codec (not Vorbis or Opus)";
    }

cleanup:
    ogg_stream_clear(&os);
    ogg_sync_clear(&oy);
    return result;
}

// ---- MP3 validation via FFmpeg -------------------------------------

// Custom AVIOContext read callback from memory buffer
struct FfmpegMemBuf {
    const uint8_t* data;
    size_t         len;
    size_t         pos = 0;
};

static int ffmpeg_read_cb(void* opaque, uint8_t* buf, int size) {
    auto* m = static_cast<FfmpegMemBuf*>(opaque);
    if (m->pos >= m->len) return AVERROR_EOF;
    int to_read = static_cast<int>(std::min<size_t>(size, m->len - m->pos));
    std::memcpy(buf, m->data + m->pos, to_read);
    m->pos += to_read;
    return to_read;
}

static int64_t ffmpeg_seek_cb(void* opaque, int64_t offset, int whence) {
    auto* m = static_cast<FfmpegMemBuf*>(opaque);
    int64_t new_pos = 0;
    if (whence == SEEK_SET)         new_pos = offset;
    else if (whence == SEEK_CUR)    new_pos = static_cast<int64_t>(m->pos) + offset;
    else if (whence == SEEK_END)    new_pos = static_cast<int64_t>(m->len) + offset;
    else if (whence == AVSEEK_SIZE) return static_cast<int64_t>(m->len);
    else return -1;
    if (new_pos < 0 || static_cast<size_t>(new_pos) > m->len) return -1;
    m->pos = static_cast<size_t>(new_pos);
    return new_pos;
}

AudioValidationResult validate_mp3_internal(const uint8_t* data, size_t len) {
    AudioValidationResult result;

    FfmpegMemBuf mem{data, len};

    constexpr int IO_BUF_SIZE = 65536;
    uint8_t* io_buf = static_cast<uint8_t*>(av_malloc(IO_BUF_SIZE));
    if (!io_buf) { result.error = "av_malloc failed"; return result; }

    AVIOContext* avio = avio_alloc_context(
        io_buf, IO_BUF_SIZE,
        0,        // write_flag
        &mem,
        ffmpeg_read_cb,
        nullptr,  // write_packet
        ffmpeg_seek_cb);

    if (!avio) {
        av_free(io_buf);
        result.error = "avio_alloc_context failed";
        return result;
    }

    AVFormatContext* fmt = avformat_alloc_context();
    fmt->pb = avio;

    int ret = avformat_open_input(&fmt, nullptr, nullptr, nullptr);
    if (ret < 0) {
        av_free(avio->buffer);
        avio_context_free(&avio);
        result.error = "avformat_open_input failed";
        return result;
    }

    ret = avformat_find_stream_info(fmt, nullptr);
    if (ret < 0) {
        avformat_close_input(&fmt);
        av_free(avio->buffer);
        avio_context_free(&avio);
        result.error = "avformat_find_stream_info failed";
        return result;
    }

    // Find audio stream and verify it is MP3
    int audio_idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_idx < 0) {
        avformat_close_input(&fmt);
        av_free(avio->buffer);
        avio_context_free(&avio);
        result.error = "no audio stream found";
        return result;
    }

    AVStream* stream = fmt->streams[audio_idx];
    AVCodecParameters* params = stream->codecpar;

    if (params->codec_id != AV_CODEC_ID_MP3) {
        avformat_close_input(&fmt);
        av_free(avio->buffer);
        avio_context_free(&avio);
        result.error = "not an MP3 stream";
        return result;
    }

    result.info.codec       = AudioCodec::MP3;
    // FFmpeg 5.1+ replaced AVCodecParameters::channels with ch_layout.
    // Stay portable with Ubuntu 22.04's FFmpeg 4.4.
#if LIBAVCODEC_VERSION_MAJOR >= 59
    result.info.channels    = params->ch_layout.nb_channels;
#else
    result.info.channels    = params->channels;
#endif
    result.info.sample_rate = params->sample_rate;

    // Duration from container (in AV_TIME_BASE units = microseconds)
    if (fmt->duration > 0) {
        result.info.duration_ms = static_cast<uint32_t>(fmt->duration / 1000);
    } else if (stream->duration > 0 && stream->time_base.den > 0) {
        double secs = static_cast<double>(stream->duration) *
                      stream->time_base.num / stream->time_base.den;
        result.info.duration_ms = static_cast<uint32_t>(secs * 1000.0);
    }

    result.valid = true;

    avformat_close_input(&fmt);
    // Note: avio->buffer was already freed by avformat_close_input when it
    // takes ownership; only free avio context itself.
    avio_context_free(&avio);
    return result;
}

} // anonymous namespace

// ---- Public API -----------------------------------------------------

AudioValidationResult validate_audio(const uint8_t* data, size_t len) {
    AudioValidationResult result;

    if (len == 0) {
        result.error = "empty data";
        return result;
    }

    if (is_ogg_magic(data, len)) {
        result = validate_ogg_internal(data, len);
    } else if (is_mp3_magic(data, len)) {
        result = validate_mp3_internal(data, len);
    } else {
        result.error = "unrecognized audio format (expected Ogg or MP3)";
        return result;
    }

    if (result.valid) {
        if (result.info.duration_ms < 30000) {
            result.valid = false;
            result.error = "duration under 30 seconds";
        } else if (result.info.channels < 1 || result.info.channels > 255) {
            result.valid = false;
            result.error = "invalid channel count";
        } else if (result.info.codec == AudioCodec::VORBIS &&
                   (result.info.sample_rate < 8000 || result.info.sample_rate > 192000)) {
            result.valid = false;
            result.error = "invalid Vorbis sample rate";
        }
    }

    return result;
}

} // namespace mc::audio
