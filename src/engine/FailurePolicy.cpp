#include "engine/FailurePolicy.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <string>

namespace ldm::engine {

namespace {

using core::FailureClass;
using core::FailureReason;
using core::TransferFailure;

TransferFailure Make(FailureReason reason, FailureClass failureClass, std::string message, int httpStatus = 0)
{
    return TransferFailure{reason, failureClass, httpStatus, std::move(message)};
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

// The media type without parameters, lower case: "text/html; charset=utf-8" -> "text/html".
std::string MediaType(std::string_view contentType)
{
    const size_t semicolon = contentType.find(';');
    std::string_view type = contentType.substr(0, semicolon);
    while (!type.empty() && (type.back() == ' ' || type.back() == '\t')) {
        type.remove_suffix(1);
    }
    while (!type.empty() && (type.front() == ' ' || type.front() == '\t')) {
        type.remove_prefix(1);
    }
    return ToLower(type);
}

bool IsHtml(std::string_view contentType)
{
    const std::string type = MediaType(contentType);
    return type == "text/html" || type == "application/xhtml+xml";
}

// Extension of the last path segment of a URL, lower case, without the dot. Empty when none.
std::string UrlExtension(std::string_view url)
{
    const size_t scheme = url.find("://");
    if (scheme != std::string_view::npos) {
        url.remove_prefix(scheme + 3);
    }
    url = url.substr(0, url.find_first_of("?#"));
    const size_t slash = url.find('/');
    if (slash == std::string_view::npos) {
        return {};
    }
    std::string_view path = url.substr(slash);
    path = path.substr(path.rfind('/') + 1);
    const size_t dot = path.rfind('.');
    if (dot == std::string_view::npos || dot + 1 == path.size()) {
        return {};
    }
    return ToLower(path.substr(dot + 1));
}

constexpr std::array<std::string_view, 12> kPageExtensions{
    "htm", "html", "xhtml", "shtml", "php", "asp", "aspx", "jsp", "cgi", "pl", "cfm", "do",
};

}  // namespace

std::optional<core::TransferFailure> ClassifyCurlResult(int curlCode, std::string_view detail)
{
    const auto code = static_cast<CURLcode>(curlCode);
    std::string message = curl_easy_strerror(code);
    if (!detail.empty()) {
        message += ": ";
        message += detail;
    }
    switch (code) {
    case CURLE_OK:
        return std::nullopt;
    case CURLE_COULDNT_RESOLVE_HOST:
        return Make(FailureReason::Dns, FailureClass::Retryable, std::move(message));
    case CURLE_COULDNT_CONNECT:
        return Make(FailureReason::ConnectionRefused, FailureClass::Retryable, std::move(message));
    case CURLE_OPERATION_TIMEDOUT:
        return Make(FailureReason::Timeout, FailureClass::Retryable, std::move(message));
    case CURLE_COULDNT_RESOLVE_PROXY:
        return Make(FailureReason::Proxy, FailureClass::Retryable, std::move(message));
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_PARTIAL_FILE:
    case CURLE_GOT_NOTHING:
    case CURLE_HTTP2:
    case CURLE_HTTP2_STREAM:
    case CURLE_FTP_ACCEPT_TIMEOUT:
    case CURLE_FTP_WEIRD_PASV_REPLY:
    case CURLE_FTP_CANT_GET_HOST:
    case CURLE_FTP_ACCEPT_FAILED:
    case CURLE_SSL_CONNECT_ERROR:
        return Make(FailureReason::ConnectionReset, FailureClass::Retryable, std::move(message));
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
        return Make(FailureReason::Tls, FailureClass::Fatal, std::move(message));
    case CURLE_LOGIN_DENIED:
        return Make(FailureReason::AuthRequired, FailureClass::NeedsUser, std::move(message));
    case CURLE_REMOTE_FILE_NOT_FOUND:
        return Make(FailureReason::NotFound, FailureClass::Fatal, std::move(message));
    case CURLE_REMOTE_ACCESS_DENIED:
        return Make(FailureReason::PermissionDenied, FailureClass::NeedsUser, std::move(message));
    case CURLE_FTP_COULDNT_USE_REST:
    case CURLE_RANGE_ERROR:
        return Make(FailureReason::ResumeUnsupported, FailureClass::NeedsUser, std::move(message));
    case CURLE_UNSUPPORTED_PROTOCOL:
    case CURLE_URL_MALFORMAT:
        return Make(FailureReason::Protocol, FailureClass::Fatal, std::move(message));
    case CURLE_OUT_OF_MEMORY:
        return Make(FailureReason::Internal, FailureClass::Fatal, std::move(message));
    default:
        return Make(FailureReason::Protocol, FailureClass::Retryable, std::move(message));
    }
}

std::optional<core::TransferFailure> ClassifyHttpStatus(int status)
{
    if (status >= 200 && status < 300) {
        return std::nullopt;
    }
    const std::string message = "HTTP status " + std::to_string(status);
    switch (status) {
    case 401:
        return Make(FailureReason::AuthRequired, FailureClass::NeedsUser, message, status);
    case 407:
        return Make(FailureReason::ProxyAuthRequired, FailureClass::NeedsUser, message, status);
    case 403:
        return Make(FailureReason::PermissionDenied, FailureClass::Fatal, message, status);
    case 404:
    case 410:
        return Make(FailureReason::NotFound, FailureClass::Fatal, message, status);
    case 416:
        return Make(FailureReason::ResourceChanged, FailureClass::NeedsUser, message, status);
    case 408:
    case 425:
    case 429:
        return Make(FailureReason::HttpError, FailureClass::Retryable, message, status);
    default:
        break;
    }
    if (status >= 500 && status < 600) {
        return Make(FailureReason::HttpError, FailureClass::Retryable, message, status);
    }
    return Make(FailureReason::HttpError, FailureClass::Fatal, message, status);
}

bool IsUnexpectedHtml(std::string_view url, std::optional<std::string_view> contentType,
                      std::optional<std::string_view> previousContentType)
{
    if (!contentType || !IsHtml(*contentType)) {
        return false;
    }
    if (previousContentType && !previousContentType->empty()) {
        return !IsHtml(*previousContentType);
    }
    const std::string extension = UrlExtension(url);
    if (extension.empty()) {
        return false;
    }
    return std::find(kPageExtensions.begin(), kPageExtensions.end(), extension) == kPageExtensions.end();
}

std::chrono::milliseconds RetryDelay(int attempt, double jitter, std::optional<int> retryAfterSeconds)
{
    constexpr int kCapSeconds = 30;
    const int exponent = std::clamp(attempt - 1, 0, 5);
    const int baseSeconds = std::min(1 << exponent, kCapSeconds);
    jitter = std::clamp(jitter, 0.75, 1.25);
    auto delay = std::chrono::milliseconds(static_cast<std::int64_t>(baseSeconds * 1000 * jitter));
    if (retryAfterSeconds && *retryAfterSeconds > 0) {
        delay = std::max(delay, std::chrono::milliseconds(std::int64_t{*retryAfterSeconds} * 1000));
    }
    return delay;
}

}  // namespace ldm::engine
