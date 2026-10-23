#include "media/UrlReference.h"

#include <algorithm>

namespace ldm::media {

namespace {

// An address split as RFC 3986 appendix B does. A missing part differs from an empty one
// ("http://a/b?" has an empty query), which section 5.3 needs to put the address back together.
struct Parts {
    std::optional<std::string_view> scheme;
    std::optional<std::string_view> authority;
    std::string_view path;
    std::optional<std::string_view> query;
    std::optional<std::string_view> fragment;
};

Parts Split(std::string_view text)
{
    Parts parts;
    const size_t schemeEnd = text.find_first_of(":/?#");
    if (schemeEnd != std::string_view::npos && schemeEnd > 0 && text[schemeEnd] == ':') {
        parts.scheme = text.substr(0, schemeEnd);
        text.remove_prefix(schemeEnd + 1);
    }
    if (text.starts_with("//")) {
        text.remove_prefix(2);
        const size_t authorityEnd = std::min(text.find_first_of("/?#"), text.size());
        parts.authority = text.substr(0, authorityEnd);
        text.remove_prefix(authorityEnd);
    }
    if (const size_t hash = text.find('#'); hash != std::string_view::npos) {
        parts.fragment = text.substr(hash + 1);
        text = text.substr(0, hash);
    }
    if (const size_t question = text.find('?'); question != std::string_view::npos) {
        parts.query = text.substr(question + 1);
        text = text.substr(0, question);
    }
    parts.path = text;
    return parts;
}

void DropLastSegment(std::string& output)
{
    const size_t slash = output.rfind('/');
    output.erase(slash == std::string::npos ? 0 : slash);
}

// RFC 3986 section 5.2.4, step by step.
std::string RemoveDotSegments(std::string_view input)
{
    std::string output;
    while (!input.empty()) {
        if (input.starts_with("../")) {
            input.remove_prefix(3);
        } else if (input.starts_with("./")) {
            input.remove_prefix(2);
        } else if (input.starts_with("/./")) {
            input.remove_prefix(2);
        } else if (input == "/.") {
            input = "/";
        } else if (input.starts_with("/../")) {
            input.remove_prefix(3);
            DropLastSegment(output);
        } else if (input == "/..") {
            input = "/";
            DropLastSegment(output);
        } else if (input == "." || input == "..") {
            input = {};
        } else {
            const size_t next = std::min(input.find('/', 1), input.size());
            output.append(input.substr(0, next));
            input.remove_prefix(next);
        }
    }
    return output;
}

// RFC 3986 section 5.2.3.
std::string Merge(const Parts& base, std::string_view referencePath)
{
    if (base.authority && base.path.empty()) {
        return "/" + std::string(referencePath);
    }
    const size_t slash = base.path.rfind('/');
    std::string merged(slash == std::string_view::npos ? std::string_view{} : base.path.substr(0, slash + 1));
    merged.append(referencePath);
    return merged;
}

bool HasControlCharacter(std::string_view text)
{
    return std::any_of(text.begin(), text.end(), [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte < 0x20 || byte == 0x7F;
    });
}

std::string LowerAscii(std::string_view text)
{
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; });
    return lower;
}

std::string EncodeSpaces(std::string_view text)
{
    std::string encoded;
    encoded.reserve(text.size());
    for (const char c : text) {
        if (c == ' ') {
            encoded.append("%20");
        } else {
            encoded.push_back(c);
        }
    }
    return encoded;
}

}  // namespace

std::optional<std::string> ResolveReference(std::string_view base, std::string_view reference)
{
    const Parts baseParts = Split(base);
    if (!baseParts.scheme) {
        return std::nullopt;
    }
    const Parts ref = Split(reference);

    // RFC 3986 section 5.2.2, strict: a reference with a scheme is taken as it is.
    std::string scheme;
    std::optional<std::string> authority;
    std::string path;
    std::optional<std::string_view> query;
    if (ref.scheme) {
        scheme = *ref.scheme;
        authority = ref.authority;
        path = RemoveDotSegments(ref.path);
        query = ref.query;
    } else {
        scheme = *baseParts.scheme;
        if (ref.authority) {
            authority = ref.authority;
            path = RemoveDotSegments(ref.path);
            query = ref.query;
        } else {
            authority = baseParts.authority;
            if (ref.path.empty()) {
                path = baseParts.path;
                query = ref.query ? ref.query : baseParts.query;
            } else {
                path = RemoveDotSegments(ref.path.starts_with('/') ? ref.path : Merge(baseParts, ref.path));
                query = ref.query;
            }
        }
    }

    // RFC 3986 section 5.3.
    std::string result = scheme + ":";
    if (authority) {
        result += "//" + *authority;
    }
    result += path;
    if (query) {
        result += "?";
        result += *query;
    }
    if (ref.fragment) {
        result += "#";
        result += *ref.fragment;
    }
    return result;
}

std::optional<std::string> ResolveMediaUrl(std::string_view base, std::string_view reference)
{
    if (HasControlCharacter(base) || HasControlCharacter(reference)) {
        return std::nullopt;
    }
    auto resolved = ResolveReference(EncodeSpaces(base), EncodeSpaces(reference));
    if (!resolved) {
        return std::nullopt;
    }
    const Parts parts = Split(*resolved);
    const std::string scheme = LowerAscii(parts.scheme.value_or(""));
    if (scheme != "http" && scheme != "https") {
        return std::nullopt;
    }
    // The host is what follows any user information and precedes any port.
    const std::string_view authority = parts.authority.value_or("");
    const size_t at = authority.rfind('@');
    const std::string_view hostAndPort = at == std::string_view::npos ? authority : authority.substr(at + 1);
    if (hostAndPort.empty() || hostAndPort.front() == ':') {
        return std::nullopt;
    }
    std::string url = scheme + "://" + std::string(authority) + std::string(parts.path);
    if (parts.query) {
        url += "?";
        url += *parts.query;
    }
    return url;
}

}  // namespace ldm::media
