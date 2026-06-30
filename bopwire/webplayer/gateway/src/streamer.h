// streamer.h — fetch a song's bytes from the swarm via the gateway's RatsLink.
//
// stream.open → seeder list + delivery_id + (optional) manifest, then a loop of
// swarm.fetch ranges reassembled from binary frames. Resumes across seeders on
// failure. Returns the whole file plus the seeder/mini used (for reward lanes).
#pragma once

#include <cstdint>
#include <string>

#include "rats_link.h"

namespace bopwire::gw {

struct FetchResult {
    bool        ok = false;
    std::string bytes;        // assembled audio file
    std::string seeder;       // peer that served the bytes (reward lane)
    std::string mini;         // relay mini node (reward lane)
    std::string delivery_id;  // broker-minted, for relay triangulation
    std::string error;        // when !ok: no_swarm | unknown | fetch_failed | …
};

class Streamer {
public:
    explicit Streamer(RatsLink& link) : link_(link) {}

    // Blocking: pull the full file for content_hash. Picks a full node, opens the
    // stream, fetches every range, and returns the bytes. Large files are bounded
    // by max_bytes (0 = default cap).
    FetchResult fetch(const std::string& content_hash, size_t max_bytes = 0);

private:
    RatsLink& link_;
};

} // namespace bopwire::gw
