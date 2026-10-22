#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ldm::engine {

// `Content-Range: bytes first-last/total` (RFC 9110 14.4). `last` is inclusive; total may be "*".
struct ContentRange {
    std::int64_t first = 0;
    std::int64_t last = 0;
    std::optional<std::int64_t> total;
};

// Headers of one HTTP response that the engine cares about, collected line by line.
struct ResponseHeaders {
    int status = 0;
    std::optional<std::int64_t> contentLength;
    std::optional<ContentRange> contentRange;
    std::optional<std::string> etag;
    std::optional<std::string> lastModified;
    std::optional<std::string> contentType;
    std::optional<std::string> contentEncoding;  // lower case; absent or "identity" means raw bytes
    std::optional<std::string> fileName;         // from Content-Disposition, decoded, not yet sanitized
    std::optional<int> retryAfterSeconds;
    bool acceptRanges = false;  // "Accept-Ranges: bytes"

    // Applies one raw header line as delivered by curl (including CRLF). A status line starts a new
    // response, so headers of redirect hops are discarded.
    void ApplyLine(std::string_view line);

    [[nodiscard]] bool IsStrongEtag() const noexcept;
    [[nodiscard]] bool HasContentEncoding() const noexcept;
};

[[nodiscard]] std::optional<ContentRange> ParseContentRange(std::string_view value);

// Extracts the file name from a Content-Disposition value (RFC 6266). Prefers `filename*`
// (RFC 8187, UTF-8 or ISO-8859-1 percent-encoded) over `filename`. Returns UTF-8.
[[nodiscard]] std::optional<std::string> ParseContentDispositionFileName(std::string_view value);

// Percent-decodes a URL path segment into raw bytes. Invalid escapes are kept literally.
[[nodiscard]] std::string PercentDecode(std::string_view text);

}  // namespace ldm::engine
