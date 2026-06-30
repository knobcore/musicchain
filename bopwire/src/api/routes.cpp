#include "routes.h"
#include <sstream>

namespace mc::api {

std::vector<std::string> parse_path(const std::string& url) {
    // Strip /api/v1 prefix if present
    std::string path = url;
    if (path.substr(0, 7) == "/api/v1") path = path.substr(7);
    if (path.empty()) path = "/";

    std::vector<std::string> segments;
    std::istringstream ss(path);
    std::string segment;
    while (std::getline(ss, segment, '/')) {
        if (!segment.empty()) segments.push_back(segment);
    }
    return segments;
}

} // namespace mc::api
