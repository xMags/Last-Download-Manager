#include "model/Cookie.h"

#include <algorithm>

namespace ldm::model {

namespace {

constexpr size_t kMaxDomain = 253;
constexpr size_t kMaxPath = 2048;
constexpr size_t kMaxName = 4096;
constexpr size_t kMaxValue = 8192;

bool HasNoControlCharacters(std::string_view text)
{
    return std::none_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; });
}

bool IsHostName(std::string_view domain)
{
    return !domain.empty() && domain.size() <= kMaxDomain && domain.front() != '.' &&
           std::all_of(domain.begin(), domain.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                      c == '-' || c == '_';
           });
}

}  // namespace

bool IsValidCookie(const Cookie& cookie) noexcept
{
    return IsHostName(cookie.domain) && !cookie.path.empty() && cookie.path.front() == '/' &&
           cookie.path.size() <= kMaxPath && HasNoControlCharacters(cookie.path) && !cookie.name.empty() &&
           cookie.name.size() <= kMaxName && HasNoControlCharacters(cookie.name) && cookie.value.size() <= kMaxValue &&
           HasNoControlCharacters(cookie.value) && cookie.expires >= 0;
}

std::string ToNetscapeLine(const Cookie& cookie)
{
    // domain, include subdomains, path, secure, expiry, name, value. A leading dot on the domain
    // is how the format marks a cookie that subdomains also receive.
    std::string line = cookie.hostOnly ? cookie.domain : "." + cookie.domain;
    line += cookie.hostOnly ? "\tFALSE\t" : "\tTRUE\t";
    line += cookie.path;
    line += cookie.secure ? "\tTRUE\t" : "\tFALSE\t";
    line += std::to_string(cookie.expires);
    line += '\t';
    line += cookie.name;
    line += '\t';
    line += cookie.value;
    return line;
}

std::string CookiesToText(std::span<const Cookie> cookies)
{
    std::string text;
    for (const Cookie& cookie : cookies) {
        text += ToNetscapeLine(cookie);
        text += '\n';
    }
    return text;
}

std::vector<std::string> CookieLines(std::string_view text)
{
    std::vector<std::string> lines;
    while (!text.empty()) {
        const size_t end = text.find('\n');
        const std::string_view line = text.substr(0, end);
        if (!line.empty()) {
            lines.emplace_back(line);
        }
        if (end == std::string_view::npos) {
            break;
        }
        text.remove_prefix(end + 1);
    }
    return lines;
}

}  // namespace ldm::model
