// mc_decoder.dll — Windows audio decoder for the player's Fingerprinter.
//
// Wraps the Windows Media Foundation Source Reader to turn any installed
// audio file (mp3, m4a, wav, flac, ogg via the MF Ogg parser when the user
// has it; otherwise WAV/MP3/AAC out of the box) into interleaved 16-bit
// signed PCM. Chromaprint takes raw int16 samples, so this is exactly what
// fingerprinting wants.
//
// Exposed C API (used by lib/src/services/fingerprinter.dart via dart:ffi):
//
//   int32_t mc_decoder_open(const wchar_t* path, mc_decoded** out);
//     0 on success, negative HRESULT on failure. Populates `out` with a
//     heap-allocated struct holding interleaved 16-bit PCM samples plus
//     the sample rate and channel count.
//
//   void mc_decoder_free(mc_decoded* d);
//     Frees the buffer returned by mc_decoder_open. Safe to call on null.
//
// The Source Reader runs synchronously here — fingerprinting blocks the
// scanner anyway, and the per-file cost is dominated by the chromaprint
// pass, not the decode.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <comdef.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "propsys.lib")

#define MC_DECODER_API extern "C" __declspec(dllexport)

struct mc_decoded {
    int16_t* pcm;          // interleaved 16-bit LE
    int64_t  sample_count; // = total int16 elements / channel_count
    int32_t  sample_rate;  // e.g. 44100
    int32_t  channel_count;
};

namespace {

// Returns false if (and only if) we successfully forced a 16-bit PCM
// output format on `reader`. We do not down-mix to mono here: chromaprint
// can handle multi-channel input directly via chromaprint_start(rate, ch).
bool ConfigurePcmOutput(IMFSourceReader* reader, int32_t* out_rate,
                        int32_t* out_channels) {
    HRESULT hr;
    IMFMediaType* target = nullptr;
    hr = MFCreateMediaType(&target);
    if (FAILED(hr)) return false;
    target->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    target->SetGUID(MF_MT_SUBTYPE,    MFAudioFormat_PCM);
    target->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);

    hr = reader->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, target);
    target->Release();
    if (FAILED(hr)) return false;

    IMFMediaType* actual = nullptr;
    hr = reader->GetCurrentMediaType(
        MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual);
    if (FAILED(hr) || !actual) return false;
    UINT32 rate = 0, channels = 0;
    actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS,       &channels);
    actual->Release();
    if (rate == 0 || channels == 0) return false;
    *out_rate     = static_cast<int32_t>(rate);
    *out_channels = static_cast<int32_t>(channels);
    return true;
}

// Ensure MF + COM are initialized exactly once per process. The init has
// no shutdown counterpart on purpose: the player runs for the lifetime
// of the process and MFShutdown forces all readers to drop, which is not
// the threading model we want when scans run on background isolates.
std::atomic<bool> g_init_done{false};
HRESULT EnsureInit() {
    bool expected = false;
    if (!g_init_done.compare_exchange_strong(expected, true)) {
        return S_OK;
    }
    HRESULT hr = CoInitializeEx(nullptr,
        COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    // S_FALSE means "already initialized on this thread"; that's fine.
    if (hr != S_OK && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        return hr;
    }
    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    return hr;
}

} // namespace

MC_DECODER_API int32_t mc_decoder_open(const wchar_t* path,
                                       mc_decoded** out) {
    if (!path || !out) return E_POINTER;
    *out = nullptr;

    HRESULT hr = EnsureInit();
    if (FAILED(hr)) return hr;

    IMFAttributes* attrs = nullptr;
    hr = MFCreateAttributes(&attrs, 1);
    if (FAILED(hr)) return hr;
    // Low-latency mode: we're scanning, no need for jitter buffering.
    attrs->SetUINT32(MF_LOW_LATENCY, TRUE);

    IMFSourceReader* reader = nullptr;
    hr = MFCreateSourceReaderFromURL(path, attrs, &reader);
    attrs->Release();
    if (FAILED(hr) || !reader) return hr ? hr : E_FAIL;

    int32_t rate = 0, channels = 0;
    if (!ConfigurePcmOutput(reader, &rate, &channels)) {
        reader->Release();
        return E_INVALIDARG;
    }

    std::vector<int16_t> samples;
    samples.reserve(rate * channels * 30); // ~30 s headroom; grows as needed.

    for (;;) {
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr)) { if (sample) sample->Release(); break; }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (sample) sample->Release();
            break;
        }
        if (!sample) continue; // gap or format change with no payload

        IMFMediaBuffer* buf = nullptr;
        hr = sample->ConvertToContiguousBuffer(&buf);
        if (FAILED(hr) || !buf) { sample->Release(); continue; }

        BYTE* bytes = nullptr;
        DWORD cur = 0, max = 0;
        hr = buf->Lock(&bytes, &max, &cur);
        if (SUCCEEDED(hr) && bytes && cur > 0) {
            const size_t add = cur / sizeof(int16_t);
            const size_t base = samples.size();
            samples.resize(base + add);
            std::memcpy(samples.data() + base, bytes, add * sizeof(int16_t));
        }
        if (SUCCEEDED(hr)) buf->Unlock();
        buf->Release();
        sample->Release();
    }
    reader->Release();

    if (samples.empty()) return MF_E_END_OF_STREAM;

    mc_decoded* d = static_cast<mc_decoded*>(std::malloc(sizeof(mc_decoded)));
    if (!d) return E_OUTOFMEMORY;
    d->pcm = static_cast<int16_t*>(
        std::malloc(samples.size() * sizeof(int16_t)));
    if (!d->pcm) { std::free(d); return E_OUTOFMEMORY; }
    std::memcpy(d->pcm, samples.data(), samples.size() * sizeof(int16_t));
    d->sample_count  = static_cast<int64_t>(samples.size());
    d->sample_rate   = rate;
    d->channel_count = channels;
    *out = d;
    return S_OK;
}

MC_DECODER_API void mc_decoder_free(mc_decoded* d) {
    if (!d) return;
    if (d->pcm) std::free(d->pcm);
    std::free(d);
}
