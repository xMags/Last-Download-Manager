#include "grabber/LocalPaths.h"

#include "core/FileNames.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace ldm::grabber {

namespace {

int HexValue(char c) noexcept
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

}  // namespace

std::string PercentDecode(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && HexValue(text[i + 1]) >= 0 && HexValue(text[i + 2]) >= 0) {
            out.push_back(static_cast<char>(HexValue(text[i + 1]) * 16 + HexValue(text[i + 2])));
            i += 2;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

namespace {

std::vector<std::string_view> Split(std::string_view text, char separator)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find(separator, start), text.size());
        parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

// FNV-1a: a short, stable tag for a query, not a security measure.
std::string QueryTag(std::string_view query)
{
    std::uint32_t hash = 2166136261u;
    for (const char c : query) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619u;
    }
    constexpr std::array<char, 16> kDigits{'0', '1', '2', '3', '4', '5', '6', '7',
                                           '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string tag = "_";
    for (int shift = 28; shift >= 0; shift -= 4) {
        tag.push_back(kDigits[(hash >> shift) & 0xF]);
    }
    return tag;
}

bool IsUnreserved(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~';
}

void AppendEncoded(std::string& out, char c)
{
    constexpr std::array<char, 16> kDigits{'0', '1', '2', '3', '4', '5', '6', '7',
                                           '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    const auto byte = static_cast<unsigned char>(c);
    out.push_back('%');
    out.push_back(kDigits[byte >> 4]);
    out.push_back(kDigits[byte & 0xF]);
}

}  // namespace

std::string LocalRelativePath(const core::Url& url, bool page, bool keepSiteFolders)
{
    std::vector<std::string_view> segments = Split(url.path, '/');
    // The path starts with '/', so the first part is empty; the last is the name, empty for a folder.
    const std::string_view lastSegment = segments.empty() ? std::string_view{} : segments.back();
    std::string name = lastSegment.empty() ? std::string() : core::SanitizeFileName(PercentDecode(lastSegment));
    if (page) {
        if (name.empty()) {
            name = "index.html";
        } else {
            const std::string extension = core::FileExtension(name);
            if (extension != "html" && extension != "htm") {
                name += ".html";
            }
        }
    } else if (name.empty()) {
        name = "index";
    }
    if (!url.query.empty()) {
        const std::size_t dot = name.rfind('.');
        const std::string tag = QueryTag(url.query);
        if (dot == std::string::npos || dot == 0) {
            name += tag;
        } else {
            name.insert(dot, tag);
        }
    }
    if (!keepSiteFolders) {
        return name;
    }
    std::string host = url.host;
    // A scheme's own port names the same site as no port, as NormalizeAddress has it too.
    const bool defaultPort = url.port && ((url.scheme == "http" && *url.port == 80) ||
                                          (url.scheme == "https" && *url.port == 443) ||
                                          (url.scheme == "ftp" && *url.port == 21) ||
                                          (url.scheme == "ftps" && *url.port == 990));
    if (url.port && !defaultPort) {
        host += "_" + std::to_string(*url.port);
    }
    std::string path = core::SanitizeFileName(host);
    for (std::size_t i = 1; i + 1 < segments.size(); ++i) {
        if (segments[i].empty()) {
            continue;
        }
        path += "/" + core::SanitizeFileName(PercentDecode(segments[i]));
    }
    return path + "/" + name;
}

std::string RelativeReference(std::string_view fromPath, std::string_view toPath)
{
    const std::vector<std::string_view> from = Split(fromPath, '/');
    const std::vector<std::string_view> to = Split(toPath, '/');
    // Folders in common, not counting the names at the ends.
    std::size_t common = 0;
    while (common + 1 < from.size() && common + 1 < to.size() && from[common] == to[common]) {
        ++common;
    }
    std::string reference;
    for (std::size_t i = common; i + 1 < from.size(); ++i) {
        reference += "../";
    }
    for (std::size_t i = common; i < to.size(); ++i) {
        for (const char c : to[i]) {
            if (IsUnreserved(c)) {
                reference.push_back(c);
            } else {
                AppendEncoded(reference, c);
            }
        }
        if (i + 1 < to.size()) {
            reference.push_back('/');
        }
    }
    return reference;
}

std::optional<std::string> ResolveLocalReference(std::string_view fromPath, std::string_view reference)
{
    if (reference.empty() || reference.front() == '/' || reference.find_first_of(":?#") != std::string_view::npos) {
        return std::nullopt;
    }
    std::vector<std::string> path;
    const std::vector<std::string_view> from = Split(fromPath, '/');
    for (std::size_t i = 0; i + 1 < from.size(); ++i) {  // the page's folder, without its name
        path.emplace_back(from[i]);
    }
    for (const std::string_view part : Split(reference, '/')) {
        if (part == "..") {
            if (path.empty()) {
                return std::nullopt;
            }
            path.pop_back();
        } else if (!part.empty() && part != ".") {
            path.push_back(PercentDecode(part));
        }
    }
    if (path.empty()) {
        return std::nullopt;
    }
    std::string joined;
    for (const std::string& part : path) {
        joined += (joined.empty() ? "" : "/") + part;
    }
    return joined;
}

std::string AttributeSafeAddress(std::string_view address)
{
    std::string out;
    out.reserve(address.size());
    for (const char c : address) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '&') {
            // An entity, not %26: the address must reach the server as it was, and %26 would join
            // two query parameters into one.
            out += "&amp;";
        } else if (byte <= 0x20 || byte >= 0x7F || c == '"' || c == '\'' || c == '<' || c == '>' || c == ',') {
            AppendEncoded(out, c);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

}  // namespace ldm::grabber
