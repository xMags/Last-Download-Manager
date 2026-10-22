#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ldm::model {

// Credentials for a server or server path (checklist O-07). The secret is DPAPI-protected.
struct SiteLogin {
    std::int64_t id = 0;
    std::string pattern;  // host[/path], '*' wildcards
    std::string username;
    std::vector<std::uint8_t> secret;
};

// Per-server connection cap (checklist ENG-04).
struct ServerLimit {
    std::string pattern;  // host, '*' wildcards
    int maxConnections = 0;
};

}  // namespace ldm::model
