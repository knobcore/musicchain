// streamer.cpp — see streamer.h.
#include "streamer.h"

#include <algorithm>
#include <cstring>

namespace bopwire::gw {

namespace {
constexpr int kPieceSize     = 256 * 1024;
constexpr int kInitialPieces = 2;    // small first fetch => fast click-to-play
constexpr int kBatchPieces   = 16;   // == kMaxSwarmFetchPieces; prefetch ahead

// Per-range wait: a single seeder flow is capped ~500 KB/s; size off 200 KB/s.
int wait_ms_for(int64_t range_len) { return static_cast<int>(range_len / 200 + 8000); }

std::string sniff_audio(const std::string& b) {
    auto eq = [&](size_t off, const char* sig, size_t n) {
        return b.size() >= off + n && std::memcmp(b.data() + off, sig, n) == 0;
    };
    if (eq(0, "ID3", 3)) return "audio/mpeg";
    if (b.size() >= 2 && uint8_t(b[0]) == 0xFF && (uint8_t(b[1]) & 0xE0) == 0xE0) return "audio/mpeg";
    if (eq(0, "fLaC", 4)) return "audio/flac";
    if (eq(0, "OggS", 4)) return "audio/ogg";
    if (eq(0, "RIFF", 4) && eq(8, "WAVE", 4)) return "audio/wav";
    if (eq(4, "ftyp", 4)) return "audio/mp4";
    return "application/octet-stream";
}
} // namespace

bool PieceStore::open() {
    std::lock_guard<std::mutex> lk(m_);
    node_ = link_.pick_full_node();
    if (node_.empty()) return false;
    mini_ = link_.pick_mini();
    if (mini_.empty()) return false;

    json o;
    try {
        o = link_.rpc_relay(mini_, node_, "stream.open", json{{"content_hash", hash_}}, 10000);
    } catch (...) { return false; }

    const json body = o.value("body", json::object());
    for (const auto& p : body.value("peers", json::array())) {
        if (p.is_object()) { auto pid = p.value("peer_id", ""); if (!pid.empty()) seeders_.push_back(pid); }
        else if (p.is_string()) seeders_.push_back(p.get<std::string>());
    }
    if (seeders_.empty()) return false;

    delivery_id_ = body.value("delivery_id", "");
    if (body.contains("manifest") && body["manifest"].is_object())
        piece_size_ = body["manifest"].value("piece_size", kPieceSize);
    if (piece_size_ <= 0 || piece_size_ > 512 * 1024) piece_size_ = kPieceSize;

    if (!fetch_batch(0, kInitialPieces)) return false;
    auto it = pieces_.find(0);
    if (it != pieces_.end()) content_type_ = sniff_audio(it->second);
    return total_size_ > 0;
}

// Fetch [piece_start, piece_start+count) as one swarm.fetch range; split the
// returned contiguous bytes into the piece cache. Tries seeders in turn.
bool PieceStore::fetch_batch(int piece_start, int count) {
    for (const std::string& seeder : seeders_) {
        const uint32_t csid = link_.next_stream_id();
        auto sink = link_.register_stream(csid);
        json fr;
        try {
            fr = link_.rpc_relay(mini_, seeder, "swarm.fetch", json{
                {"v", 1}, {"content_hash", hash_}, {"piece_size", piece_size_},
                {"piece_start", piece_start}, {"count", count},
                {"delivery_id", delivery_id_}, {"client_stream_id", csid},
            }, 12000);
        } catch (...) { link_.unregister_stream(csid); continue; }

        const json fb = fr.value("body", json::object());
        if (fb.value("status", fr.value("status", std::string())) != "ok") {
            link_.unregister_stream(csid); continue;
        }
        total_size_ = fb.value("total_size", total_size_);
        const int64_t range_len = fb.value("range_length", 0);
        const uint32_t rsid = fb.value("stream_id", csid);
        if (rsid != csid) link_.alias_stream(rsid, sink);

        const bool done = sink->wait_done(wait_ms_for(range_len));
        std::string bytes;
        { std::lock_guard<std::mutex> lk(sink->m); bytes.swap(sink->out); }
        link_.unregister_stream(csid);
        if (rsid != csid) link_.unregister_stream(rsid);

        if (!done || static_cast<int64_t>(bytes.size()) < range_len) continue;  // next seeder

        for (size_t off = 0, i = 0; off < bytes.size(); off += piece_size_, ++i) {
            const int idx = piece_start + static_cast<int>(i);
            const size_t n = std::min<size_t>(piece_size_, bytes.size() - off);
            if (!pieces_.count(idx)) { pieces_[idx] = bytes.substr(off, n); cached_bytes_ += n; }
        }
        seeder_ = seeder;
        return true;
    }
    return false;
}

std::string PieceStore::get_range(int64_t offset, int64_t len) {
    std::lock_guard<std::mutex> lk(m_);
    if (total_size_ < 0 || offset >= total_size_) return {};
    const int64_t end = std::min(offset + len, total_size_);
    const int first = static_cast<int>(offset / piece_size_);
    const int last  = static_cast<int>((end - 1) / piece_size_);

    for (int p = first; p <= last; ++p) {
        if (!pieces_.count(p)) {
            // fetch a batch starting at the missing piece (prefetch ahead)
            if (!fetch_batch(p, kBatchPieces)) return {};
            if (!pieces_.count(p)) return {};
        }
    }
    std::string out;
    out.reserve(static_cast<size_t>(end - offset));
    for (int p = first; p <= last; ++p) {
        const std::string& pc = pieces_[p];
        const int64_t pstart = static_cast<int64_t>(p) * piece_size_;
        const int64_t s = std::max(offset, pstart) - pstart;
        const int64_t e = std::min(end, pstart + static_cast<int64_t>(pc.size())) - pstart;
        if (e > s) out.append(pc, static_cast<size_t>(s), static_cast<size_t>(e - s));
    }
    return out;
}

} // namespace bopwire::gw
