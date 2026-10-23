#include "media/UrlReference.h"

#include <gtest/gtest.h>

#include <string_view>
#include <utility>

namespace {

using ldm::media::ResolveMediaUrl;
using ldm::media::ResolveReference;

// RFC 3986 section 5.4, against its base address.
constexpr std::string_view kRfcBase = "http://a/b/c/d;p?q";

TEST(UrlReference, ResolvesTheRfcNormalExamples)
{
    const std::pair<std::string_view, std::string_view> examples[] = {
        {"g:h", "g:h"},
        {"g", "http://a/b/c/g"},
        {"./g", "http://a/b/c/g"},
        {"g/", "http://a/b/c/g/"},
        {"/g", "http://a/g"},
        {"//g", "http://g"},
        {"?y", "http://a/b/c/d;p?y"},
        {"g?y", "http://a/b/c/g?y"},
        {"#s", "http://a/b/c/d;p?q#s"},
        {"g#s", "http://a/b/c/g#s"},
        {"g?y#s", "http://a/b/c/g?y#s"},
        {";x", "http://a/b/c/;x"},
        {"g;x", "http://a/b/c/g;x"},
        {"g;x?y#s", "http://a/b/c/g;x?y#s"},
        {"", "http://a/b/c/d;p?q"},
        {".", "http://a/b/c/"},
        {"./", "http://a/b/c/"},
        {"..", "http://a/b/"},
        {"../", "http://a/b/"},
        {"../g", "http://a/b/g"},
        {"../..", "http://a/"},
        {"../../", "http://a/"},
        {"../../g", "http://a/g"},
    };
    for (const auto& [reference, expected] : examples) {
        EXPECT_EQ(ResolveReference(kRfcBase, reference), expected) << reference;
    }
}

TEST(UrlReference, ResolvesTheRfcAbnormalExamples)
{
    const std::pair<std::string_view, std::string_view> examples[] = {
        {"../../../g", "http://a/g"},
        {"../../../../g", "http://a/g"},
        {"/./g", "http://a/g"},
        {"/../g", "http://a/g"},
        {"g.", "http://a/b/c/g."},
        {".g", "http://a/b/c/.g"},
        {"g..", "http://a/b/c/g.."},
        {"..g", "http://a/b/c/..g"},
        {"./../g", "http://a/b/g"},
        {"./g/.", "http://a/b/c/g/"},
        {"g/./h", "http://a/b/c/g/h"},
        {"g/../h", "http://a/b/c/h"},
        {"g;x=1/./y", "http://a/b/c/g;x=1/y"},
        {"g;x=1/../y", "http://a/b/c/y"},
        {"g?y/./x", "http://a/b/c/g?y/./x"},
        {"g?y/../x", "http://a/b/c/g?y/../x"},
        {"g#s/./x", "http://a/b/c/g#s/./x"},
        {"g#s/../x", "http://a/b/c/g#s/../x"},
        {"http:g", "http:g"},  // strict parsers, as section 5.4.2 recommends
    };
    for (const auto& [reference, expected] : examples) {
        EXPECT_EQ(ResolveReference(kRfcBase, reference), expected) << reference;
    }
}

TEST(UrlReference, NeedsAnAbsoluteBase)
{
    EXPECT_FALSE(ResolveReference("/relative/base", "g"));
    EXPECT_EQ(ResolveReference("http://a", "g"), "http://a/g");  // empty base path (section 5.2.3)
}

TEST(UrlReference, MediaAddressesAreHttpWithAHost)
{
    const std::string_view base = "https://cdn.example.test/live/master.m3u8?token=1";
    EXPECT_EQ(ResolveMediaUrl(base, "720p/index.m3u8"), "https://cdn.example.test/live/720p/index.m3u8");
    EXPECT_EQ(ResolveMediaUrl(base, ""), "https://cdn.example.test/live/master.m3u8?token=1");
    EXPECT_EQ(ResolveMediaUrl(base, "HTTP://Other.test/a.ts#t=1"), "http://Other.test/a.ts");
    EXPECT_EQ(ResolveMediaUrl(base, "seg 1.ts"), "https://cdn.example.test/live/seg%201.ts");
    EXPECT_EQ(ResolveMediaUrl(base, "//other.test:8080/x.ts"), "https://other.test:8080/x.ts");

    for (const char* reference : {"file:///C:/Windows/win.ini", "javascript:alert(1)", "data:text/plain,abc",
                                  "skd://key", "ftp://a.test/x.ts", "http:///nohost.ts", "http://:80/x.ts",
                                  "http://user@/x.ts", "seg\x01.ts", "seg\n.ts"}) {
        EXPECT_FALSE(ResolveMediaUrl(base, reference)) << reference;
    }
    EXPECT_FALSE(ResolveMediaUrl("file:///C:/media/index.m3u8", "seg.ts"));
}

}  // namespace
