#include "core/Url.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>

namespace ldm::core {

namespace {

constexpr std::array<std::string_view, 4> kSchemes{"http", "https", "ftp", "ftps"};

char ToLowerAscii(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string ToLower(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), ToLowerAscii);
    return result;
}

bool IsSpace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool IsHostChar(char c) noexcept
{
    const auto byte = static_cast<unsigned char>(c);
    // Letters, digits, '-', '.', '_' and any non-ASCII byte (international names, encoded by curl).
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || byte >= 0x80;
}

// Characters that end an address found in free text.
bool EndsUrlInText(char c) noexcept
{
    return IsSpace(c) || c == '"' || c == '<' || c == '>' || c == '\'' || c == '`' || c == '{' || c == '}' || c == '|' ||
           c == '\\' || c == '^';
}

}  // namespace

std::string Url::SiteRoot() const
{
    std::string root = scheme + "://";
    root += host.find(':') != std::string::npos ? "[" + host + "]" : host;
    if (port) {
        root += ":" + std::to_string(*port);
    }
    root += "/";
    return root;
}

std::optional<Url> ParseUrl(std::string_view text)
{
    while (!text.empty() && IsSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && IsSpace(text.back())) {
        text.remove_suffix(1);
    }
    if (std::any_of(text.begin(), text.end(), [](char c) { return IsSpace(c) || static_cast<unsigned char>(c) < 0x20; })) {
        return std::nullopt;
    }
    const size_t schemeEnd = text.find("://");
    if (schemeEnd == std::string_view::npos) {
        return std::nullopt;
    }
    Url url;
    url.scheme = ToLower(text.substr(0, schemeEnd));
    if (std::find(kSchemes.begin(), kSchemes.end(), url.scheme) == kSchemes.end()) {
        return std::nullopt;
    }
    std::string_view rest = text.substr(schemeEnd + 3);
    const size_t authorityEnd = rest.find_first_of("/?#");
    std::string_view authority = rest.substr(0, authorityEnd);
    rest = authorityEnd == std::string_view::npos ? std::string_view{} : rest.substr(authorityEnd);

    if (const size_t at = authority.rfind('@'); at != std::string_view::npos) {
        url.hasUserInfo = true;
        authority.remove_prefix(at + 1);
    }
    std::string_view host = authority;
    std::string_view port;
    if (!authority.empty() && authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        host = authority.substr(1, close - 1);
        const std::string_view after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') {
                return std::nullopt;
            }
            port = after.substr(1);
        }
        if (host.empty() || !std::all_of(host.begin(), host.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == ':' || c == '.';
            })) {
            return std::nullopt;
        }
    } else {
        if (const size_t colon = authority.rfind(':'); colon != std::string_view::npos) {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        }
        if (host.empty() || !std::all_of(host.begin(), host.end(), IsHostChar) || host.front() == '.' ||
            host.find("..") != std::string_view::npos) {
            return std::nullopt;
        }
    }
    url.host = ToLower(host);
    if (!port.empty()) {
        int value = 0;
        const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), value);
        if (error != std::errc{} || end != port.data() + port.size() || value < 1 || value > 65535) {
            return std::nullopt;
        }
        url.port = value;
    }

    rest = rest.substr(0, rest.find('#'));
    const size_t question = rest.find('?');
    url.path = std::string(rest.substr(0, question));
    if (url.path.empty()) {
        url.path = "/";
    }
    if (question != std::string_view::npos) {
        url.query = std::string(rest.substr(question + 1));
    }
    return url;
}

std::string WithQueryParameters(std::string_view url, std::string_view query)
{
    const auto split = [](std::string_view text) {
        std::vector<std::string_view> parts;
        while (!text.empty()) {
            const size_t amp = text.find('&');
            if (const std::string_view part = text.substr(0, amp); !part.empty()) {
                parts.push_back(part);
            }
            if (amp == std::string_view::npos) {
                break;
            }
            text.remove_prefix(amp + 1);
        }
        return parts;
    };
    const auto name = [](std::string_view part) { return part.substr(0, part.find('=')); };
    const std::vector<std::string_view> added = split(query);
    if (added.empty()) {
        return std::string(url);
    }
    const size_t hash = url.find('#');
    const std::string_view fragment = hash == std::string_view::npos ? std::string_view() : url.substr(hash);
    std::string_view rest = url.substr(0, hash);
    const size_t mark = rest.find('?');
    std::string address(rest.substr(0, mark));
    std::vector<std::string_view> parts =
        mark == std::string_view::npos ? std::vector<std::string_view>() : split(rest.substr(mark + 1));
    std::erase_if(parts, [&](std::string_view part) {
        return std::any_of(added.begin(), added.end(), [&](std::string_view a) { return name(a) == name(part); });
    });
    parts.insert(parts.end(), added.begin(), added.end());
    for (size_t i = 0; i < parts.size(); ++i) {
        address += i == 0 ? '?' : '&';
        address += parts[i];
    }
    address += fragment;
    return address;
}

std::vector<std::string> ExtractUrls(std::string_view text)
{
    std::vector<std::string> result;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t best = std::string_view::npos;
        for (const std::string_view scheme : kSchemes) {
            for (size_t found = text.find(scheme, pos); found != std::string_view::npos;
                 found = text.find(scheme, found + 1)) {
                // Must be followed by "://" and start a word.
                if (text.substr(found + scheme.size(), 3) != "://") {
                    continue;
                }
                const bool wordStart = found == 0 || !std::isalnum(static_cast<unsigned char>(text[found - 1]));
                if (wordStart) {
                    best = std::min(best, found);
                    break;
                }
            }
        }
        if (best == std::string_view::npos) {
            break;
        }
        size_t end = best;
        while (end < text.size() && !EndsUrlInText(text[end])) {
            ++end;
        }
        std::string_view candidate = text.substr(best, end - best);
        // Trailing punctuation usually belongs to the sentence, not the address.
        while (!candidate.empty() && std::string_view(".,;:!?)]").find(candidate.back()) != std::string_view::npos) {
            candidate.remove_suffix(1);
        }
        if (ParseUrl(candidate) && std::find(result.begin(), result.end(), candidate) == result.end()) {
            result.emplace_back(candidate);
        }
        pos = end;
    }
    return result;
}

}  // namespace ldm::core
