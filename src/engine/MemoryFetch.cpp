#include "engine/MemoryFetch.h"

#include "engine/FailurePolicy.h"

#include <utility>

namespace ldm::engine {

MemoryFetch::MemoryFetch(CURLM* multi, core::FetchRequest request, long connectTimeoutSeconds,
                         long stallTimeoutSeconds)
    : multi_(multi), request_(std::move(request)), easy_(curl_easy_init())
{
    CURL* easy = easy_.get();
    if (easy == nullptr) {
        return;
    }
    curl_easy_setopt(easy, CURLOPT_URL, request_.url.c_str());
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, connectTimeoutSeconds);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, stallTimeoutSeconds);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, static_cast<long>(kTotalTimeout.count()));
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, error_.data());
    curl_easy_setopt(easy, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
    // Manifests are text and compress well; libcurl decodes whatever it offered.
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(easy, CURLOPT_USERAGENT, request_.userAgent.c_str());
    if (request_.referer) {
        curl_easy_setopt(easy, CURLOPT_REFERER, request_.referer->c_str());
    }
    curl_easy_setopt(easy, CURLOPT_PROXY, request_.proxy ? request_.proxy->c_str() : "");
    if (!request_.cookieLines.empty()) {
        curl_easy_setopt(easy, CURLOPT_COOKIEFILE, "");  // turns on the cookie engine without reading a file
        for (const std::string& line : request_.cookieLines) {
            curl_easy_setopt(easy, CURLOPT_COOKIELIST, line.c_str());  // copied by libcurl
        }
    }
    curl_easy_setopt(easy, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(request_.maxBytes));
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &MemoryFetch::OnWrite);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, this);
    added_ = curl_multi_add_handle(multi_, easy) == CURLM_OK;
}

MemoryFetch::~MemoryFetch()
{
    if (added_) {
        curl_multi_remove_handle(multi_, easy_.get());
    }
}

size_t MemoryFetch::OnWrite(char* data, size_t size, size_t count, void* userdata)
{
    auto* self = static_cast<MemoryFetch*>(userdata);
    const size_t bytes = size * count;
    if (self->body_.size() + bytes > self->request_.maxBytes) {
        self->tooLarge_ = true;
        return 0;  // ends the transfer with CURLE_WRITE_ERROR
    }
    self->body_.append(data, bytes);
    return bytes;
}

core::FetchFinished MemoryFetch::Finish(CURLcode result)
{
    core::FetchFinished finished;
    finished.fetchId = request_.fetchId;
    if (!added_) {
        finished.failure = core::TransferFailure{core::FailureReason::Internal, core::FailureClass::Fatal, 0,
                                                 "the request could not be started"};
        return finished;
    }
    CURL* easy = easy_.get();
    if (tooLarge_ || result == CURLE_FILESIZE_EXCEEDED) {
        finished.failure = core::TransferFailure{core::FailureReason::Protocol, core::FailureClass::Fatal, 0,
                                                 "the manifest is larger than " + std::to_string(request_.maxBytes) +
                                                     " bytes"};
        return finished;
    }
    if (auto failure = ClassifyCurlResult(static_cast<int>(result), error_.data())) {
        finished.failure = std::move(failure);
        return finished;
    }
    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    if (auto failure = ClassifyHttpStatus(static_cast<int>(status))) {
        finished.failure = std::move(failure);
        return finished;
    }
    const char* effective = nullptr;
    if (curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &effective) == CURLE_OK && effective != nullptr) {
        finished.finalUrl = effective;
    } else {
        finished.finalUrl = request_.url;
    }
    const char* contentType = nullptr;
    if (curl_easy_getinfo(easy, CURLINFO_CONTENT_TYPE, &contentType) == CURLE_OK && contentType != nullptr) {
        finished.contentType = std::string(contentType);
    }
    finished.body = std::move(body_);
    return finished;
}

}  // namespace ldm::engine
