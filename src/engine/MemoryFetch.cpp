#include "engine/MemoryFetch.h"

#include "core/NetworkAddress.h"
#include "core/Url.h"
#include "engine/FailurePolicy.h"

#include <algorithm>
#include <string_view>
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
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, static_cast<long>(request_.totalTimeout.value_or(kTotalTimeout).count()));
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, error_.data());
    curl_easy_setopt(easy, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
    // Manifests are text and compress well; libcurl decodes whatever it offered.
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(easy, CURLOPT_USERAGENT, request_.userAgent.c_str());
    if (request_.referer) {
        curl_easy_setopt(easy, CURLOPT_REFERER, request_.referer->c_str());
    }
    curl_easy_setopt(easy, CURLOPT_PROXY, request_.proxy ? request_.proxy->c_str() : "");
    if (request_.credentials) {
        // Copied by libcurl, which sends them to the first host only (no CURLOPT_UNRESTRICTED_AUTH).
        curl_easy_setopt(easy, CURLOPT_USERNAME, request_.credentials->user.c_str());
        curl_easy_setopt(easy, CURLOPT_PASSWORD, request_.credentials->password.c_str());
        curl_easy_setopt(easy, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_ANY));
    }
    if (request_.headOnly) {
        curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
    }
    if (!request_.cookieLines.empty()) {
        curl_easy_setopt(easy, CURLOPT_COOKIEFILE, "");  // turns on the cookie engine without reading a file
        for (const std::string& line : request_.cookieLines) {
            curl_easy_setopt(easy, CURLOPT_COOKIELIST, line.c_str());  // copied by libcurl
        }
    }
    // Through a proxy the connection goes to the proxy, which says nothing about the server.
    addressGuard_.refuse = request_.refusePrivateAddresses && !request_.proxy;
    InstallAddressGuard(easy, &addressGuard_);
    // libcurl refuses an announced length over the limit before any byte arrives; with body types
    // the type decides first, so a large file is reported as skipped rather than too large. The
    // write callback holds the limit either way.
    // A HEAD request has no body to limit; its Content-Length may be any size.
    if (request_.bodyTypes.empty() && !request_.headOnly) {
        curl_easy_setopt(easy, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(request_.maxBytes));
    }
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
    if (!self->typeChecked_) {
        self->typeChecked_ = true;
        if (!self->WantsBody()) {
            self->bodySkipped_ = true;
            return 0;  // ends the transfer with CURLE_WRITE_ERROR, which Finish reports as a success
        }
    }
    if (self->body_.size() + bytes > self->request_.maxBytes) {
        self->tooLarge_ = true;
        return 0;  // ends the transfer with CURLE_WRITE_ERROR
    }
    self->body_.append(data, bytes);
    return bytes;
}

bool MemoryFetch::WantsBody() const
{
    if (request_.bodyTypes.empty()) {
        return true;
    }
    const char* contentType = nullptr;
    if (curl_easy_getinfo(easy_.get(), CURLINFO_CONTENT_TYPE, &contentType) != CURLE_OK || contentType == nullptr) {
        return false;
    }
    std::string_view type(contentType);
    type = type.substr(0, type.find(';'));
    while (!type.empty() && (type.back() == ' ' || type.back() == '	')) {
        type.remove_suffix(1);
    }
    return std::any_of(request_.bodyTypes.begin(), request_.bodyTypes.end(), [type](const std::string& wanted) {
        return wanted.size() == type.size() &&
               std::equal(wanted.begin(), wanted.end(), type.begin(), [](char a, char b) {
                   const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
                   return lower(a) == lower(b);
               });
    });
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
    if (addressGuard_.refuse && addressGuard_.reachedPrivate) {
        finished.failure = core::TransferFailure{core::FailureReason::PrivateAddress, core::FailureClass::Fatal, 0,
                                                 "the address leads to a loopback or private network address"};
        return finished;
    }
    if (tooLarge_ || result == CURLE_FILESIZE_EXCEEDED) {
        finished.failure = core::TransferFailure{core::FailureReason::Protocol, core::FailureClass::Fatal, 0,
                                                 "the response is larger than " + std::to_string(request_.maxBytes) +
                                                     " bytes"};
        return finished;
    }
    if (bodySkipped_ && result == CURLE_WRITE_ERROR) {
        result = CURLE_OK;
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
    curl_off_t length = -1;
    if (curl_easy_getinfo(easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length) == CURLE_OK && length >= 0) {
        finished.contentLength = static_cast<std::int64_t>(length);
    }
    finished.bodySkipped = bodySkipped_;
    finished.body = std::move(body_);
    const auto url = core::ParseUrl(finished.finalUrl);
    finished.fromPrivateNetwork = (!request_.proxy && addressGuard_.reachedPrivate) || (url && core::IsPrivateHost(url->host));
    return finished;
}

}  // namespace ldm::engine
