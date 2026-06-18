#include "fingerprint.h"
#include "ogg_decoder.h"
#include "multi_decoder.h"
#include <chromaprint.h>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#ifdef _MSC_VER
#include <intrin.h>
static inline int portable_popcount(unsigned int x) { return static_cast<int>(__popcnt(x)); }
#else
static inline int portable_popcount(unsigned int x) { return __builtin_popcount(x); }
#endif

namespace mc::audio {

// ---- Base64 helpers (OpenSSL) ---------------------------------------

std::string base64_encode(const uint8_t* data, size_t len) {
    BIO* b64  = BIO_new(BIO_f_base64());
    BIO* bmem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, bmem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data, static_cast<int>(len));
    BIO_flush(b64);
    BUF_MEM* bptr = nullptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string result(bptr->data, bptr->length);
    BIO_free_all(b64);
    return result;
}

std::vector<uint8_t> base64_decode(const std::string& s) {
    BIO* b64  = BIO_new(BIO_f_base64());
    BIO* bmem = BIO_new_mem_buf(s.data(), static_cast<int>(s.size()));
    b64 = BIO_push(b64, bmem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    std::vector<uint8_t> out(s.size());
    int len = BIO_read(b64, out.data(), static_cast<int>(out.size()));
    BIO_free_all(b64);
    if (len < 0) return {};
    out.resize(static_cast<size_t>(len));
    return out;
}

// ---- Fingerprint implementation ------------------------------------

std::unique_ptr<Fingerprint> Fingerprint::from_any(const uint8_t* data,
                                                    size_t len) {
    auto pcm = decode_any(data, len);
    if (pcm.samples.empty() || pcm.sample_rate <= 0 || pcm.channels <= 0)
        return nullptr;

    ChromaprintContext* ctx = chromaprint_new(CHROMAPRINT_ALGORITHM_DEFAULT);
    if (!ctx) return nullptr;
    chromaprint_start(ctx, pcm.sample_rate, pcm.channels);
    // chromaprint_feed takes int16 samples = nb_samples * channels.
    chromaprint_feed(ctx, pcm.samples.data(),
                     static_cast<int>(pcm.samples.size()));
    chromaprint_finish(ctx);

    uint32_t* fp_data = nullptr;
    int       fp_size = 0;
    chromaprint_get_raw_fingerprint(ctx, &fp_data, &fp_size);
    auto result = std::make_unique<Fingerprint>();
    if (fp_size > 0) {
        result->raw_.assign(fp_data, fp_data + fp_size);
        chromaprint_dealloc(fp_data);
    }
    chromaprint_free(ctx);
    return result;
}

std::unique_ptr<Fingerprint> Fingerprint::from_ogg(const uint8_t* data, size_t len) {
    auto decoder = OggDecoder::open(data, len);
    int  rate    = decoder->sample_rate();
    int  ch      = decoder->channels();

    ChromaprintContext* ctx = chromaprint_new(CHROMAPRINT_ALGORITHM_DEFAULT);
    if (!ctx) throw std::runtime_error("chromaprint_new failed");

    chromaprint_start(ctx, rate, ch);

    static constexpr int BUF_SAMPLES = 4096;
    std::vector<int16_t> buf(BUF_SAMPLES);
    int read = 0;
    while ((read = decoder->read(buf.data(), BUF_SAMPLES)) > 0) {
        chromaprint_feed(ctx, buf.data(), read);
    }
    chromaprint_finish(ctx);

    uint32_t* fp_data = nullptr;
    int       fp_size = 0;
    chromaprint_get_raw_fingerprint(ctx, &fp_data, &fp_size);

    auto result = std::make_unique<Fingerprint>();
    if (fp_size > 0) {
        result->raw_.assign(fp_data, fp_data + fp_size);
        chromaprint_dealloc(fp_data);
    }
    chromaprint_free(ctx);
    return result;
}

std::unique_ptr<Fingerprint> Fingerprint::from_compressed(const std::string& encoded) {
    // The blob is chromaprint's own base64-style text (URL-safe `-_`,
    // not standard base64 `+/`) — what `chromaprint_get_fingerprint`
    // and `chromaprint_encode_fingerprint(...base64=1)` produce. We
    // hand it straight to the decoder with base64=1.
    //
    // Defensive validation: reject blobs that contain characters
    // outside chromaprint's URL-safe alphabet. The old (pre-fix)
    // pipeline wrapped chromaprint output in OpenSSL base64, producing
    // `+/=` characters. Feeding that to chromaprint_decode_fingerprint
    // has been observed to crash the full node under load (the decoder
    // doesn't always handle malformed input gracefully). Filtering up
    // front turns crashes into clean no-ops.
    if (encoded.empty() || encoded.size() > 4 * 1024 * 1024) return nullptr;
    for (unsigned char c : encoded) {
        const bool ok = (c >= 'A' && c <= 'Z')
                     || (c >= 'a' && c <= 'z')
                     || (c >= '0' && c <= '9')
                     || c == '-' || c == '_';
        if (!ok) return nullptr;
    }
    uint32_t* fp_data = nullptr;
    int       fp_size = 0;
    if (chromaprint_decode_fingerprint(
            encoded.data(), static_cast<int>(encoded.size()),
            &fp_data, &fp_size, nullptr, 1) != 1) {
        return nullptr;
    }
    if (!fp_data || fp_size <= 0) {
        if (fp_data) chromaprint_dealloc(fp_data);
        return nullptr;
    }
    auto result = std::make_unique<Fingerprint>();
    result->raw_.assign(fp_data, fp_data + fp_size);
    chromaprint_dealloc(fp_data);
    return result;
}

std::string Fingerprint::compressed() const {
    char* encoded = nullptr;
    int   enc_len = 0;
    chromaprint_encode_fingerprint(
        raw_.data(), static_cast<int>(raw_.size()),
        CHROMAPRINT_ALGORITHM_DEFAULT,
        &encoded, &enc_len, 1);
    std::string result;
    if (encoded) {
        result.assign(encoded, encoded + enc_len);
        chromaprint_dealloc(encoded);
    }
    return result;
}

float Fingerprint::similarity(const Fingerprint& other) const {
    const auto& a = raw_;
    const auto& b = other.raw_;
    if (a.empty() || b.empty()) return 0.0f;

    // Find best offset alignment (search over [-offset_range, +offset_range])
    int   best_matching = 0;
    int   best_len      = 0;
    int   offset_range  = std::min(static_cast<int>(a.size()),
                                   static_cast<int>(b.size())) / 4;
    if (offset_range < 1) offset_range = 1;

    for (int off = -offset_range; off <= offset_range; ++off) {
        int ai_start = std::max(0, -off);
        int bi_start = std::max(0,  off);
        int length = static_cast<int>(std::min(a.size() - ai_start,
                                               b.size() - bi_start));
        if (length <= 0) continue;

        int matching = 0;
        for (int i = 0; i < length; ++i) {
            // Count matching bits (allow 2 bit errors per hash)
            uint32_t diff = a[ai_start + i] ^ b[bi_start + i];
            int bits = portable_popcount(diff);
            if (bits <= 2) ++matching;
        }
        if (length > best_len || matching > best_matching) {
            best_matching = matching;
            best_len      = length;
        }
    }

    if (best_len == 0) return 0.0f;
    return static_cast<float>(best_matching) / static_cast<float>(best_len);
}

std::vector<uint16_t> Fingerprint::bucket_ids() const {
    if (raw_.empty()) return {};
    std::vector<uint16_t> buckets;
    int n     = static_cast<int>(raw_.size());
    int count = std::min(n, 100);
    buckets.reserve(count);
    for (int i = 0; i < count; ++i) {
        int idx = (i * n) / count;
        uint16_t bucket = static_cast<uint16_t>(raw_[idx] >> 16);
        buckets.push_back(bucket);
    }
    // Remove duplicates
    std::sort(buckets.begin(), buckets.end());
    buckets.erase(std::unique(buckets.begin(), buckets.end()), buckets.end());
    return buckets;
}

} // namespace mc::audio
