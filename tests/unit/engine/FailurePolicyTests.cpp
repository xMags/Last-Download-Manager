#include "engine/FailurePolicy.h"

#include <curl/curl.h>
#include <gtest/gtest.h>

namespace {

using ldm::core::FailureClass;
using ldm::core::FailureReason;
using ldm::engine::ClassifyCurlResult;
using ldm::engine::ClassifyHttpStatus;
using ldm::engine::IsUnexpectedHtml;
using ldm::engine::RetryDelay;
using std::chrono::milliseconds;

TEST(FailurePolicy, CurlResultsMapToDistinctReasons)
{
    EXPECT_FALSE(ClassifyCurlResult(CURLE_OK, ""));
    const struct {
        CURLcode code;
        FailureReason reason;
        FailureClass failureClass;
    } cases[] = {
        {CURLE_COULDNT_RESOLVE_HOST, FailureReason::Dns, FailureClass::Retryable},
        {CURLE_COULDNT_CONNECT, FailureReason::ConnectionRefused, FailureClass::Retryable},
        {CURLE_OPERATION_TIMEDOUT, FailureReason::Timeout, FailureClass::Retryable},
        {CURLE_RECV_ERROR, FailureReason::ConnectionReset, FailureClass::Retryable},
        {CURLE_PARTIAL_FILE, FailureReason::ConnectionReset, FailureClass::Retryable},
        {CURLE_COULDNT_RESOLVE_PROXY, FailureReason::Proxy, FailureClass::Retryable},
        {CURLE_PEER_FAILED_VERIFICATION, FailureReason::Tls, FailureClass::Fatal},
        {CURLE_LOGIN_DENIED, FailureReason::AuthRequired, FailureClass::NeedsUser},
        {CURLE_REMOTE_FILE_NOT_FOUND, FailureReason::NotFound, FailureClass::Fatal},
        {CURLE_FTP_COULDNT_USE_REST, FailureReason::ResumeUnsupported, FailureClass::NeedsUser},
    };
    for (const auto& item : cases) {
        const auto failure = ClassifyCurlResult(item.code, "detail");
        ASSERT_TRUE(failure) << item.code;
        EXPECT_EQ(failure->reason, item.reason) << item.code;
        EXPECT_EQ(failure->failureClass, item.failureClass) << item.code;
        EXPECT_NE(failure->message.find("detail"), std::string::npos);
    }
}

TEST(FailurePolicy, HttpStatuses)
{
    EXPECT_FALSE(ClassifyHttpStatus(200));
    EXPECT_FALSE(ClassifyHttpStatus(206));
    EXPECT_EQ(ClassifyHttpStatus(401)->reason, FailureReason::AuthRequired);
    EXPECT_EQ(ClassifyHttpStatus(401)->failureClass, FailureClass::NeedsUser);
    EXPECT_EQ(ClassifyHttpStatus(407)->reason, FailureReason::ProxyAuthRequired);
    EXPECT_EQ(ClassifyHttpStatus(404)->reason, FailureReason::NotFound);
    EXPECT_EQ(ClassifyHttpStatus(410)->failureClass, FailureClass::Fatal);
    EXPECT_EQ(ClassifyHttpStatus(416)->reason, FailureReason::ResourceChanged);
    EXPECT_EQ(ClassifyHttpStatus(429)->failureClass, FailureClass::Retryable);
    EXPECT_EQ(ClassifyHttpStatus(503)->failureClass, FailureClass::Retryable);
    EXPECT_EQ(ClassifyHttpStatus(400)->failureClass, FailureClass::Fatal);
    EXPECT_EQ(ClassifyHttpStatus(503)->httpStatus, 503);
}

TEST(FailurePolicy, DetectsHtmlInsteadOfFile)
{
    EXPECT_TRUE(IsUnexpectedHtml("https://x.test/a/setup.exe?token=1", "text/html; charset=utf-8", std::nullopt));
    EXPECT_FALSE(IsUnexpectedHtml("https://x.test/a/setup.exe", "application/octet-stream", std::nullopt));
    EXPECT_FALSE(IsUnexpectedHtml("https://x.test/page.html", "text/html", std::nullopt));
    EXPECT_FALSE(IsUnexpectedHtml("https://x.test/download.php?id=4", "text/html", std::nullopt));
    EXPECT_FALSE(IsUnexpectedHtml("https://x.test/get", "text/html", std::nullopt));  // no extension: unknown
    EXPECT_FALSE(IsUnexpectedHtml("https://x.test", "text/html", std::nullopt));
    // A resumed run that saw a binary type before and now gets a page.
    EXPECT_TRUE(IsUnexpectedHtml("https://x.test/get", "TEXT/HTML", std::string_view("application/zip")));
    EXPECT_FALSE(IsUnexpectedHtml("https://x.test/a.zip", "text/html", std::string_view("text/html")));
    EXPECT_FALSE(IsUnexpectedHtml("https://x.test/a.zip", std::nullopt, std::nullopt));
}

TEST(FailurePolicy, RetryDelayBacksOffAndCaps)
{
    EXPECT_EQ(RetryDelay(1, 1.0, std::nullopt), milliseconds(1000));
    EXPECT_EQ(RetryDelay(2, 1.0, std::nullopt), milliseconds(2000));
    EXPECT_EQ(RetryDelay(4, 1.0, std::nullopt), milliseconds(8000));
    EXPECT_EQ(RetryDelay(6, 1.0, std::nullopt), milliseconds(30000));
    EXPECT_EQ(RetryDelay(50, 1.0, std::nullopt), milliseconds(30000));
    EXPECT_EQ(RetryDelay(1, 0.75, std::nullopt), milliseconds(750));
    EXPECT_EQ(RetryDelay(1, 9.0, std::nullopt), milliseconds(1250));  // jitter is clamped
    EXPECT_EQ(RetryDelay(1, 1.0, 20), milliseconds(20000));           // Retry-After wins when longer
    EXPECT_EQ(RetryDelay(5, 1.0, 1), milliseconds(16000));
}

}  // namespace
