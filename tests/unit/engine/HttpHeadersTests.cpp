#include "engine/HttpHeaders.h"

#include <gtest/gtest.h>

namespace {

using ldm::engine::ParseContentDispositionFileName;
using ldm::engine::ParseContentRange;
using ldm::engine::ResponseHeaders;

TEST(ContentRange, ParsesBoundedRange)
{
    const auto range = ParseContentRange("bytes 100-199/1024");
    ASSERT_TRUE(range);
    EXPECT_EQ(range->first, 100);
    EXPECT_EQ(range->last, 199);
    EXPECT_EQ(range->total, 1024);
}

TEST(ContentRange, AcceptsUnknownTotal)
{
    const auto range = ParseContentRange("bytes 0-9/*");
    ASSERT_TRUE(range);
    EXPECT_FALSE(range->total.has_value());
}

TEST(ContentRange, RejectsMalformedValues)
{
    for (const char* value : {"bytes */1024", "bytes 200-100/1024", "bytes 0-1024/1024", "items 0-1/2", "bytes 0-x/10",
                              "bytes -5-10/20", "bytes 0-99999999999999999999/1"}) {
        EXPECT_FALSE(ParseContentRange(value)) << value;
    }
}

TEST(ContentDisposition, PrefersExtendedFileName)
{
    const auto name = ParseContentDispositionFileName(
        "attachment; filename=\"fallback.txt\"; filename*=UTF-8''%E2%82%AC%20rates.txt");
    EXPECT_EQ(name, "\xE2\x82\xAC rates.txt");
}

TEST(ContentDisposition, HandlesQuotedAndEscaped)
{
    EXPECT_EQ(ParseContentDispositionFileName("attachment; filename=\"a \\\"quoted\\\" name.zip\""),
              "a \"quoted\" name.zip");
    EXPECT_EQ(ParseContentDispositionFileName("inline;filename=plain.bin"), "plain.bin");
    EXPECT_EQ(ParseContentDispositionFileName("attachment; filename=with space.iso ; size=10"), "with space.iso");
}

TEST(ContentDisposition, ConvertsLatin1ExtendedValue)
{
    EXPECT_EQ(ParseContentDispositionFileName("attachment; filename*=iso-8859-1'en'caf%E9.txt"), "caf\xC3\xA9.txt");
}

TEST(ContentDisposition, IgnoresUnknownCharsetAndFallsBack)
{
    EXPECT_EQ(ParseContentDispositionFileName("attachment; filename*=koi8-r''%C1; filename=\"ok.txt\""), "ok.txt");
    EXPECT_FALSE(ParseContentDispositionFileName("attachment"));
    EXPECT_FALSE(ParseContentDispositionFileName("attachment; filename=\"\""));
}

TEST(ResponseHeaders, StatusLineStartsNewResponse)
{
    ResponseHeaders headers;
    headers.ApplyLine("HTTP/1.1 302 Found\r\n");
    headers.ApplyLine("Location: /next\r\n");
    headers.ApplyLine("Content-Length: 5\r\n");
    headers.ApplyLine("\r\n");
    headers.ApplyLine("HTTP/2 206\r\n");
    headers.ApplyLine("content-range: bytes 10-19/100\r\n");
    headers.ApplyLine("ETag: \"v1\"\r\n");
    headers.ApplyLine("Accept-Ranges: bytes\r\n");
    EXPECT_EQ(headers.status, 206);
    EXPECT_FALSE(headers.contentLength.has_value());
    ASSERT_TRUE(headers.contentRange);
    EXPECT_EQ(headers.contentRange->first, 10);
    EXPECT_TRUE(headers.IsStrongEtag());
    EXPECT_TRUE(headers.acceptRanges);
}

TEST(ResponseHeaders, RecognizesWeakEtagAndEncoding)
{
    ResponseHeaders headers;
    headers.ApplyLine("HTTP/1.1 200 OK");
    headers.ApplyLine("ETag: W/\"abc\"");
    headers.ApplyLine("Content-Encoding: GZIP");
    headers.ApplyLine("Retry-After: 99999");
    EXPECT_FALSE(headers.IsStrongEtag());
    EXPECT_TRUE(headers.HasContentEncoding());
    EXPECT_EQ(headers.retryAfterSeconds, 3600);
}

TEST(ResponseHeaders, IdentityEncodingIsRaw)
{
    ResponseHeaders headers;
    headers.ApplyLine("HTTP/1.1 200 OK");
    headers.ApplyLine("Content-Encoding: identity");
    EXPECT_FALSE(headers.HasContentEncoding());
}

TEST(PercentDecode, DecodesValidAndKeepsInvalid)
{
    EXPECT_EQ(ldm::engine::PercentDecode("a%20b%2Fc"), "a b/c");
    EXPECT_EQ(ldm::engine::PercentDecode("100%"), "100%");
    EXPECT_EQ(ldm::engine::PercentDecode("%zz%4"), "%zz%4");
}

}  // namespace
