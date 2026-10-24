#pragma once

#include "core/TransferEngine.h"
#include "engine/ConnectionGuards.h"
#include "engine/CurlHandles.h"

#include <curl/curl.h>

#include <array>
#include <chrono>
#include <string>

namespace ldm::engine {

// One FetchRequest on the network thread: a single GET into memory, for streaming manifests (M3
// design section 6). Not thread-safe; every method runs on the network thread. The body is
// capped at the request's maxBytes, also when the server sends more than it announced.
class MemoryFetch {
public:
    MemoryFetch(CURLM* multi, core::FetchRequest request, long connectTimeoutSeconds, long stallTimeoutSeconds);
    ~MemoryFetch();
    MemoryFetch(const MemoryFetch&) = delete;
    MemoryFetch& operator=(const MemoryFetch&) = delete;

    // False when libcurl refused the handle; Finish then reports the failure at once.
    [[nodiscard]] bool Started() const noexcept { return added_; }
    [[nodiscard]] CURL* Handle() const noexcept { return easy_.get(); }

    // The answer, once libcurl reported the handle done with `result` (or it never started).
    [[nodiscard]] core::FetchFinished Finish(CURLcode result);

    // A manifest is small; a fetch taking longer than this has stalled.
    static constexpr std::chrono::seconds kTotalTimeout{60};

private:
    static size_t OnWrite(char* data, size_t size, size_t count, void* userdata);

    CURLM* multi_;
    core::FetchRequest request_;
    CurlEasy easy_;
    std::string body_;
    bool tooLarge_ = false;
    bool added_ = false;
    std::array<char, CURL_ERROR_SIZE> error_{};
    AddressGuard addressGuard_;
};

}  // namespace ldm::engine
