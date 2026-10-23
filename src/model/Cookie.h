#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ldm::model {

// A browser cookie that belongs to a captured download (M3 design section 5, ENG-14). Only the
// cookies the browser itself would send to the download's address are ever handed over.
struct Cookie {
    std::string domain;      // without a leading dot
    bool hostOnly = true;    // false: also sent to subdomains of `domain`
    std::string path = "/";
    bool secure = false;     // only over https
    std::int64_t expires = 0;  // Unix seconds; 0 for a session cookie
    std::string name;
    std::string value;

    auto operator<=>(const Cookie&) const = default;
};

// True when every field is safe to write into a Netscape cookie line: no tabs, line breaks or
// other control characters, a plausible host name, a path starting with '/'.
[[nodiscard]] bool IsValidCookie(const Cookie& cookie) noexcept;

// One line of the Netscape cookie file format, which libcurl's cookie engine reads. libcurl then
// sends each cookie only to the hosts and paths it belongs to, also after redirects. Callers pass
// only valid cookies.
[[nodiscard]] std::string ToNetscapeLine(const Cookie& cookie);

// Every cookie as one text block (one line each), the form that is encrypted and stored.
[[nodiscard]] std::string CookiesToText(std::span<const Cookie> cookies);
// The lines of a block made by CookiesToText; empty lines are skipped.
[[nodiscard]] std::vector<std::string> CookieLines(std::string_view text);

}  // namespace ldm::model
