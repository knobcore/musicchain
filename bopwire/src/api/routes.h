#pragma once
// Route parsing utilities shared between server and routes implementation.
#include <cstdint>
#include <string>
#include <vector>

namespace mc::api {

// Split URL path into segments, ignoring leading /api/v1
std::vector<std::string> parse_path(const std::string& url);

struct RateLimit {
    uint32_t count        = 0;
    uint64_t window_start = 0;
};

} // namespace mc::api
