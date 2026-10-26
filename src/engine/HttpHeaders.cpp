#include "engine/HttpHeaders.h"

#include <algorithm>
#include <charconv>

namespace ldm::engine {

namespace {

constexpr int kMaxRetryAfterSeconds = 3600;

bool IsSpace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view Trim(std::string_view text) noexcept
{
    while (!text.empty() && IsSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && IsSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

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

bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return ToLowerAscii(x) == ToLowerAscii(y); });
}

std::optional<std::int64_t> ParseNonNegative(std::string_view text) noexcept
{
    text = Trim(text);
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;  // overflow
    }
    return value;
}

int HexValue(char c) noexcept
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    const char lower = ToLowerAscii(c);
    if (lower >= 'a' && lower <= 'f') {
        return lower - 'a' + 10;
    }
    return -1;
}

std::string Latin1ToUtf8(std::string_view bytes)
{
    std::string result;
    result.reserve(bytes.size());
    for (char c : bytes) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x80) {
            result.push_back(c);
        } else {
            result.push_back(static_cast<char>(0xC0 | (byte >> 6)));
            result.push_back(static_cast<char>(0x80 | (byte & 0x3F)));
        }
    }
    return result;
}

// Reads one parameter value starting at `pos`: a quoted-string with backslash escapes, or a bare
// token running to the next ';'. Advances `pos` past the value.
std::string ReadParameterValue(std::string_view text, size_t& pos)
{
    std::string value;
    if (pos < text.size() && text[pos] == '"') {
        ++pos;
        while (pos < text.size() && text[pos] != '"') {
            if (text[pos] == '\\' && pos + 1 < text.size()) {
                ++pos;
            }
            value.push_back(text[pos]);
            ++pos;
        }
        if (pos < text.size()) {
            ++pos;  // closing quote
        }
        return value;
    }
    const size_t end = text.find(';', pos);
    const std::string_view token = text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
    pos = end == std::string_view::npos ? text.size() : end;
    return std::string(Trim(token));
}

// Decodes an RFC 8187 ext-value: charset'language'percent-encoded.
std::optional<std::string> DecodeExtValue(std::string_view value)
{
    const size_t firstQuote = value.find('\'');
    if (firstQuote == std::string_view::npos) {
        return std::nullopt;
    }
    const size_t secondQuote = value.find('\'', firstQuote + 1);
    if (secondQuote == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view charset = Trim(value.substr(0, firstQuote));
    const std::string decoded = PercentDecode(value.substr(secondQuote + 1));
    if (EqualsIgnoreCase(charset, "utf-8")) {
        return decoded;
    }
    if (EqualsIgnoreCase(charset, "iso-8859-1")) {
        return Latin1ToUtf8(decoded);
    }
    return std::nullopt;
}

}  // namespace

std::string PercentDecode(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int high = HexValue(text[i + 1]);
            const int low = HexValue(text[i + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        result.push_back(text[i]);
    }
    return result;
}

std::optional<ContentRange> ParseContentRange(std::string_view value)
{
    value = Trim(value);
    constexpr std::string_view kUnit = "bytes ";
    if (value.size() < kUnit.size() || !EqualsIgnoreCase(value.substr(0, kUnit.size()), kUnit)) {
        return std::nullopt;
    }
    value.remove_prefix(kUnit.size());
    const size_t dash = value.find('-');
    const size_t slash = value.find('/');
    if (dash == std::string_view::npos || slash == std::string_view::npos || dash > slash) {
        return std::nullopt;
    }
    const auto first = ParseNonNegative(value.substr(0, dash));
    const auto last = ParseNonNegative(value.substr(dash + 1, slash - dash - 1));
    if (!first || !last || *last < *first) {
        return std::nullopt;
    }
    ContentRange range{*first, *last, std::nullopt};
    const std::string_view totalText = Trim(value.substr(slash + 1));
    if (totalText != "*") {
        const auto total = ParseNonNegative(totalText);
        if (!total || *total <= *last) {
            return std::nullopt;
        }
        range.total = *total;
    }
    return range;
}

std::optional<std::string> ParseContentDispositionFileName(std::string_view value)
{
    std::optional<std::string> plain;
    std::optional<std::string> extended;
    size_t pos = value.find(';');
    while (pos != std::string_view::npos && pos < value.size()) {
        ++pos;  // skip ';'
        while (pos < value.size() && IsSpace(value[pos])) {
            ++pos;
        }
        const size_t equals = value.find('=', pos);
        if (equals == std::string_view::npos) {
            break;
        }
        if (const size_t next = value.find(';', pos); next < equals) {
            pos = next;  // a parameter without a value ("attachment; foo; filename=a.zip")
            continue;
        }
        const std::string name = ToLower(Trim(value.substr(pos, equals - pos)));
        pos = equals + 1;
        while (pos < value.size() && IsSpace(value[pos])) {
            ++pos;
        }
        const std::string parameter = ReadParameterValue(value, pos);
        if (name == "filename*") {
            if (auto decoded = DecodeExtValue(parameter)) {
                extended = std::move(decoded);
            }
        } else if (name == "filename") {
            plain = parameter;
        }
        pos = value.find(';', pos);
    }
    if (extended && !extended->empty()) {
        return extended;
    }
    if (plain && !plain->empty()) {
        return plain;
    }
    return std::nullopt;
}

void ResponseHeaders::ApplyLine(std::string_view line)
{
    line = Trim(line);
    if (line.empty()) {
        return;
    }
    if (line.size() >= 5 && line.substr(0, 5) == "HTTP/") {
        *this = ResponseHeaders{};
        const size_t space = line.find(' ');
        if (space != std::string_view::npos) {
            const auto code = ParseNonNegative(line.substr(space + 1, 3));
            status = code ? static_cast<int>(*code) : 0;
        }
        return;
    }
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
        return;
    }
    const std::string_view name = Trim(line.substr(0, colon));
    const std::string_view value = Trim(line.substr(colon + 1));

    if (EqualsIgnoreCase(name, "content-length")) {
        contentLength = ParseNonNegative(value);
    } else if (EqualsIgnoreCase(name, "content-range")) {
        contentRange = ParseContentRange(value);
    } else if (EqualsIgnoreCase(name, "etag")) {
        etag = std::string(value);
    } else if (EqualsIgnoreCase(name, "last-modified")) {
        lastModified = std::string(value);
    } else if (EqualsIgnoreCase(name, "content-type")) {
        contentType = std::string(value);
    } else if (EqualsIgnoreCase(name, "content-encoding")) {
        contentEncoding = ToLower(value);
    } else if (EqualsIgnoreCase(name, "content-disposition")) {
        fileName = ParseContentDispositionFileName(value);
    } else if (EqualsIgnoreCase(name, "accept-ranges")) {
        const std::string units = ToLower(value);
        acceptRanges = units.find("bytes") != std::string::npos;
        acceptRangesNone = Trim(units) == "none";
    } else if (EqualsIgnoreCase(name, "retry-after")) {
        if (const auto seconds = ParseNonNegative(value)) {
            retryAfterSeconds = static_cast<int>(std::min<std::int64_t>(*seconds, kMaxRetryAfterSeconds));
        }
    }
}

bool ResponseHeaders::IsStrongEtag() const noexcept
{
    return etag.has_value() && !etag->empty() && !(etag->size() >= 2 && (*etag)[0] == 'W' && (*etag)[1] == '/');
}

bool ResponseHeaders::HasContentEncoding() const noexcept
{
    return contentEncoding.has_value() && !contentEncoding->empty() && *contentEncoding != "identity";
}

}  // namespace ldm::engine
