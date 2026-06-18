#include "multi_decoder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cstring>

namespace mc::audio {

namespace {

// AVIO read callback — pulls from our in-memory buffer.
struct MemReader {
    const uint8_t* data;
    size_t         len;
    size_t         pos;
};

int mem_read_packet(void* opaque, uint8_t* buf, int buf_size) {
    auto* r = static_cast<MemReader*>(opaque);
    if (r->pos >= r->len) return AVERROR_EOF;
    size_t remain = r->len - r->pos;
    size_t copy   = std::min<size_t>(remain, static_cast<size_t>(buf_size));
    std::memcpy(buf, r->data + r->pos, copy);
    r->pos += copy;
    return static_cast<int>(copy);
}

int64_t mem_seek(void* opaque, int64_t offset, int whence) {
    auto* r = static_cast<MemReader*>(opaque);
    int64_t new_pos = 0;
    if (whence == AVSEEK_SIZE) return static_cast<int64_t>(r->len);
    if (whence == SEEK_SET) new_pos = offset;
    else if (whence == SEEK_CUR) new_pos =
        static_cast<int64_t>(r->pos) + offset;
    else if (whence == SEEK_END) new_pos =
        static_cast<int64_t>(r->len) + offset;
    if (new_pos < 0 || new_pos > static_cast<int64_t>(r->len))
        return -1;
    r->pos = static_cast<size_t>(new_pos);
    return new_pos;
}

} // namespace

DecodedPcm decode_any(const uint8_t* data, size_t len) {
    DecodedPcm out;
    if (!data || len == 0) return out;

    MemReader reader{data, len, 0};
    constexpr int kAvioBuf = 32 * 1024;
    auto* avio_buf = static_cast<unsigned char*>(av_malloc(kAvioBuf));
    if (!avio_buf) return out;
    AVIOContext* avio = avio_alloc_context(
        avio_buf, kAvioBuf, /*write_flag=*/0, &reader,
        mem_read_packet, /*write=*/nullptr, mem_seek);
    if (!avio) { av_free(avio_buf); return out; }

    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) { av_free(avio->buffer); avio_context_free(&avio); return out; }
    fmt->pb = avio;

    if (avformat_open_input(&fmt, nullptr, nullptr, nullptr) < 0) {
        av_free(avio->buffer); avio_context_free(&avio); return out;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt); av_free(avio->buffer);
        avio_context_free(&avio); return out;
    }

    int stream_index = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            stream_index = static_cast<int>(i);
            break;
        }
    }
    if (stream_index < 0) {
        avformat_close_input(&fmt); av_free(avio->buffer);
        avio_context_free(&avio); return out;
    }

    AVStream*       stream = fmt->streams[stream_index];
    AVCodecParameters* par = stream->codecpar;
    const AVCodec* codec   = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        avformat_close_input(&fmt); av_free(avio->buffer);
        avio_context_free(&avio); return out;
    }
    AVCodecContext* cctx = avcodec_alloc_context3(codec);
    if (!cctx) {
        avformat_close_input(&fmt); av_free(avio->buffer);
        avio_context_free(&avio); return out;
    }
    if (avcodec_parameters_to_context(cctx, par) < 0 ||
        avcodec_open2(cctx, codec, nullptr) < 0) {
        avcodec_free_context(&cctx);
        avformat_close_input(&fmt); av_free(avio->buffer);
        avio_context_free(&avio); return out;
    }

    // Set up swresample to push everything to int16 interleaved at the
    // source rate / channel count.
    SwrContext* swr = nullptr;
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, cctx->ch_layout.nb_channels);
    if (swr_alloc_set_opts2(&swr,
            &out_layout, AV_SAMPLE_FMT_S16, cctx->sample_rate,
            &cctx->ch_layout, cctx->sample_fmt, cctx->sample_rate,
            0, nullptr) < 0 || !swr || swr_init(swr) < 0) {
        if (swr) swr_free(&swr);
        avcodec_free_context(&cctx);
        avformat_close_input(&fmt); av_free(avio->buffer);
        avio_context_free(&avio); return out;
    }

    out.sample_rate = cctx->sample_rate;
    out.channels    = cctx->ch_layout.nb_channels;

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frm = av_frame_alloc();
    if (!pkt || !frm) {
        if (pkt) av_packet_free(&pkt);
        if (frm) av_frame_free(&frm);
        swr_free(&swr);
        avcodec_free_context(&cctx);
        avformat_close_input(&fmt); av_free(avio->buffer);
        avio_context_free(&avio); return out;
    }

    std::vector<int16_t> resample_buf;
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != stream_index) { av_packet_unref(pkt); continue; }
        if (avcodec_send_packet(cctx, pkt) >= 0) {
            while (avcodec_receive_frame(cctx, frm) >= 0) {
                const int n   = frm->nb_samples;
                const int ch  = out.channels;
                resample_buf.resize(static_cast<size_t>(n) * ch);
                uint8_t* outbuf[1] = {
                    reinterpret_cast<uint8_t*>(resample_buf.data()) };
                int written = swr_convert(
                    swr, outbuf, n,
                    const_cast<const uint8_t**>(frm->data), n);
                if (written > 0) {
                    out.samples.insert(out.samples.end(),
                        resample_buf.begin(),
                        resample_buf.begin() + written * ch);
                }
                av_frame_unref(frm);
            }
        }
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    av_frame_free(&frm);
    swr_free(&swr);
    avcodec_free_context(&cctx);
    avformat_close_input(&fmt);
    av_free(avio->buffer);
    avio_context_free(&avio);
    return out;
}

} // namespace mc::audio
