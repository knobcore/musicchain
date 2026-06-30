// streamer.h — on-demand piece store for progressive audio streaming.
//
// Instead of downloading a whole song before serving (high click-to-play
// latency), a PieceStore opens the swarm stream, fetches the first piece(s) to
// learn the size + content type, then fetches further pieces ON DEMAND as the
// HTTP layer pulls bytes. Pieces are cached, so seeks/re-reads don't re-fetch.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "rats_link.h"

namespace bopwire::gw {

class PieceStore {
public:
    PieceStore(RatsLink& link, std::string content_hash)
        : link_(link), hash_(std::move(content_hash)) {}

    // stream.open + fetch the first piece(s): learns total_size, content_type,
    // seeders, and pins one mini for this play. false if no swarm / no node.
    bool open();

    // Bytes [offset, offset+len) clamped to total, fetching+caching pieces as
    // needed. Empty at EOF or on a fetch failure.
    std::string get_range(int64_t offset, int64_t len);

    int64_t            total_size()   const { return total_size_; }
    const std::string& content_type() const { return content_type_; }
    const std::string& seeder()       const { return seeder_; }   // reward lane
    const std::string& mini()         const { return mini_; }     // reward lane

private:
    bool fetch_batch(int piece_start, int count);   // caller holds m_

    RatsLink&   link_;
    std::string hash_;
    std::mutex  m_;

    int         piece_size_  = 256 * 1024;
    int64_t     total_size_  = -1;
    std::string delivery_id_;
    std::string content_type_ = "application/octet-stream";
    std::vector<std::string> seeders_;
    std::string node_, mini_, seeder_;
    std::map<int, std::string> pieces_;   // piece_index -> bytes
    size_t      cached_bytes_ = 0;
};

} // namespace bopwire::gw
