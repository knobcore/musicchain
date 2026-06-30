// streamer.cpp — see streamer.h.
#include "streamer.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace bopwire::gw {

namespace {
constexpr int     kPieceSize     = 256 * 1024;
constexpr int     kInitialPieces = 1;            // 256 KB first => fast click-to-play
constexpr int     kBatchPieces   = 8;            // 2 MB per prefetch/on-demand fetch
constexpr int64_t kAheadBytes    = 4 * 1024 * 1024;  // keep ~4 MB fetched ahead of playback

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

PieceStore::~PieceStore() {
    stop_.store(true);
    cv_.notify_all();
    if (prefetch_.joinable()) prefetch_.join();
}

std::string PieceStore::seeder() { std::lock_guard<std::mutex> lk(m_); return seeder_; }

bool PieceStore::open() {
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

    FetchOut r = swarm_fetch(0, kInitialPieces);
    if (!r.ok) return false;
    {
        std::lock_guard<std::mutex> lk(m_);
        store_locked(0, r.bytes);
        if (r.total >= 0) total_size_.store(r.total);
        seeder_ = r.seeder;
        prefetch_next_ = static_cast<int>((r.bytes.size() + piece_size_ - 1) / piece_size_);
        auto it = pieces_.find(0);
        if (it != pieces_.end()) content_type_ = sniff_audio(it->second);
    }
    if (total_size_.load() <= 0) return false;

    prefetch_ = std::thread(&PieceStore::prefetch_loop, this);
    return true;
}

// One swarm.fetch range; tries seeders in turn. No PieceStore lock held.
PieceStore::FetchOut PieceStore::swarm_fetch(int piece_start, int count) {
    FetchOut out;
    for (const std::string& seeder : seeders_) {
        if (stop_.load()) return out;
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
        const int64_t total     = fb.value("total_size", int64_t(-1));
        const int64_t range_len = fb.value("range_length", int64_t(0));
        const uint32_t rsid     = fb.value("stream_id", csid);
        if (rsid != csid) link_.alias_stream(rsid, sink);

        const bool done = sink->wait_done(wait_ms_for(range_len));
        std::string bytes;
        { std::lock_guard<std::mutex> lk(sink->m); bytes.swap(sink->out); }
        link_.unregister_stream(csid);
        if (rsid != csid) link_.unregister_stream(rsid);

        if (!done || static_cast<int64_t>(bytes.size()) < range_len) continue;  // next seeder
        out.ok = true; out.bytes = std::move(bytes); out.total = total; out.seeder = seeder;
        return out;
    }
    return out;
}

void PieceStore::store_locked(int piece_start, const std::string& bytes) {
    for (size_t off = 0, i = 0; off < bytes.size(); off += piece_size_, ++i) {
        const int idx = piece_start + static_cast<int>(i);
        const size_t n = std::min<size_t>(piece_size_, bytes.size() - off);
        if (!pieces_.count(idx)) pieces_[idx] = bytes.substr(off, n);
    }
}

// Keep fetching pieces until ~kAheadBytes past last_served_, then idle until the
// HTTP layer advances. Bounded look-ahead caps eager download + memory.
void PieceStore::prefetch_loop() {
    while (!stop_.load()) {
        int next; int64_t total, served;
        {
            std::unique_lock<std::mutex> lk(m_);
            next = prefetch_next_; total = total_size_.load(); served = last_served_;
            if (total >= 0 && static_cast<int64_t>(next) * piece_size_ >= total) {
                cv_.wait(lk, [&] { return stop_.load(); });   // fully fetched
                return;
            }
            if (static_cast<int64_t>(next) * piece_size_ >= served + kAheadBytes) {
                cv_.wait_for(lk, std::chrono::milliseconds(150), [&] {
                    return stop_.load() ||
                           last_served_ + kAheadBytes > static_cast<int64_t>(prefetch_next_) * piece_size_;
                });
                continue;
            }
        }
        FetchOut r = swarm_fetch(next, kBatchPieces);
        if (stop_.load()) break;
        std::unique_lock<std::mutex> lk(m_);
        if (!r.ok) {                                  // transient failure: back off + retry
            lk.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            continue;
        }
        store_locked(next, r.bytes);
        if (r.total >= 0) total_size_.store(r.total);
        if (seeder_.empty()) seeder_ = r.seeder;
        prefetch_next_ = next + static_cast<int>((r.bytes.size() + piece_size_ - 1) / piece_size_);
        cv_.notify_all();
    }
}

std::string PieceStore::get_range(int64_t offset, int64_t len) {
    std::unique_lock<std::mutex> lk(m_);
    const int64_t total = total_size_.load();
    if (total < 0 || offset >= total) return {};
    const int64_t end = std::min(offset + len, total);
    last_served_ = std::max(last_served_, end);
    cv_.notify_all();                                 // let the prefetcher advance its window

    const int first = static_cast<int>(offset / piece_size_);
    const int last  = static_cast<int>((end - 1) / piece_size_);
    for (int p = first; p <= last; ++p) {
        if (pieces_.count(p)) continue;
        // wait for the prefetcher to deliver it
        const bool got = cv_.wait_for(lk, std::chrono::seconds(12),
                                      [&] { return stop_.load() || pieces_.count(p) > 0; });
        if (stop_.load()) return {};
        if (!got || !pieces_.count(p)) {              // prefetcher behind → fetch now
            lk.unlock();
            FetchOut r = swarm_fetch(p, kBatchPieces);
            lk.lock();
            if (!r.ok) return {};
            store_locked(p, r.bytes);
            if (r.total >= 0) total_size_.store(r.total);
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
