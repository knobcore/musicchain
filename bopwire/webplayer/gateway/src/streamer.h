// streamer.h — on-demand piece store with a background prefetcher.
//
// open() fetches the first piece (fast click-to-play), then a background thread
// keeps fetching pieces a few MB AHEAD of what's been served, into a cache. The
// HTTP layer's get_range() therefore almost always hits cache instantly, so a
// throttling browser that pauses/resumes fetching never lands on a blocking
// swarm fetch — which was the residual mid-stream lag. On-demand fetch is the
// fallback if the prefetcher can't keep up.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rats_link.h"

namespace bopwire::gw {

class PieceStore {
public:
    PieceStore(RatsLink& link, std::string content_hash)
        : link_(link), hash_(std::move(content_hash)) {}
    ~PieceStore();

    bool open();                                   // stream.open + first piece + start prefetch
    std::string get_range(int64_t offset, int64_t len);

    // Served size = file size minus any skipped leading tag (e.g. ID3v2 art).
    int64_t            total_size()   const { const int64_t t = total_size_.load(); return t < 0 ? t : (t - audio_offset_); }
    const std::string& content_type() const { return content_type_; }
    std::string        seeder();                   // copy under lock (reward lane)
    const std::string& mini()         const { return mini_; }

private:
    struct FetchOut { bool ok = false; std::string bytes; int64_t total = -1; std::string seeder; };
    // NO lock held; reads immutable members. seeder_start rotates which seeder is
    // tried first, so concurrent fetches spread across seeders when there's > 1.
    FetchOut swarm_fetch(int piece_start, int count, int seeder_start = 0);
    void     store_locked(int piece_start, const std::string& bytes);  // m_ held
    void     prefetch_loop();

    RatsLink&   link_;
    std::string hash_;

    // set once in open() before the prefetcher starts → read without lock
    int         piece_size_   = 64 * 1024;
    int64_t     audio_offset_ = 0;        // leading bytes to skip (ID3v2 tag) so the browser gets audio first
    std::string delivery_id_;
    std::string content_type_ = "application/octet-stream";
    std::vector<std::string> seeders_;
    std::string node_, mini_;

    mutable std::mutex      m_;
    std::condition_variable cv_;
    std::map<int, std::string> pieces_;            // piece_index -> bytes
    std::atomic<int64_t>    total_size_{-1};
    int64_t                 last_served_ = 0;      // high-water byte the HTTP layer wants
    std::chrono::steady_clock::time_point last_touch_{};  // last get_range() — idle prefetch when stale
    int                     prefetch_next_ = 0;    // next piece the prefetcher will fetch
    std::string             seeder_;               // first successful seeder

    std::thread       prefetch_;
    std::atomic<bool> stop_{false};
};

} // namespace bopwire::gw
