#pragma once

#include "core/TransferEngine.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ldm::engine {

// Maps a libcurl result code (CURLcode as int, so this header stays free of curl) to a failure.
// Returns nothing for CURLE_OK.
[[nodiscard]] std::optional<core::TransferFailure> ClassifyCurlResult(int curlCode, std::string_view detail);

// Maps a final HTTP status to a failure. Returns nothing for 2xx statuses. Redirects never reach
// here because libcurl follows them.
[[nodiscard]] std::optional<core::TransferFailure> ClassifyHttpStatus(int status);

// True when a server answered with a web page where a file was expected (checklist ENG-15):
// the response is text/html and either an earlier run saw another type, or the URL path names a
// file whose extension is not a web page.
[[nodiscard]] bool IsUnexpectedHtml(std::string_view url, std::optional<std::string_view> contentType,
                                    std::optional<std::string_view> previousContentType);

// Delay before retry number `attempt` (1-based): 1, 2, 4, 8, 16 seconds, capped at 30, scaled by
// `jitter` in [0.75, 1.25]. A server's Retry-After wins when it asks for longer.
[[nodiscard]] std::chrono::milliseconds RetryDelay(int attempt, double jitter, std::optional<int> retryAfterSeconds);

}  // namespace ldm::engine
