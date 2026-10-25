#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ldm::core {

// The parts of a download address LDM needs. Only http, https, ftp and ftps are accepted.
struct Url {
    std::string scheme;  // lower case
    std::string host;    // lower case, without brackets for IPv6
    std::optional<int> port;
    std::string path;   // starts with '/', still percent-encoded
    std::string query;  // without '?'
    bool hasUserInfo = false;

    // "scheme://host[:port]/", the Referer LDM sends for manual downloads (ENG-12).
    [[nodiscard]] std::string SiteRoot() const;
};

// Parses and validates a user-entered or server-provided address (checklist A-01). Surrounding
// whitespace is ignored; anything without a supported scheme and a host is rejected.
[[nodiscard]] std::optional<Url> ParseUrl(std::string_view text);

// Finds every http(s) or ftp(s) address in free text, in order, without duplicates (checklist A-08).
[[nodiscard]] std::vector<std::string> ExtractUrls(std::string_view text);

// `url` with the parameters of `query` ("a=1&b=2") in its query: each replaces one of the same
// name that is there, the others stay. A fragment ("#...") stays at the end. Names are compared as
// written, without decoding, so the parameters must come as the site wrote them.
[[nodiscard]] std::string WithQueryParameters(std::string_view url, std::string_view query);

}  // namespace ldm::core
