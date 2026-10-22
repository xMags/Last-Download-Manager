#include "base/Utf.h"

#include <gtest/gtest.h>

#include <string>

namespace {

TEST(Utf, RoundTripsNonAscii)
{
    const std::string utf8 = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xF0\x9F\x93\xA5 file.zip";
    const std::wstring utf16 = ldm::base::ToUtf16(utf8);
    EXPECT_EQ(utf16, L"Привет \U0001F4E5 file.zip");
    EXPECT_EQ(ldm::base::ToUtf8(utf16), utf8);
}

TEST(Utf, EmptyStaysEmpty)
{
    EXPECT_TRUE(ldm::base::ToUtf16("").empty());
    EXPECT_TRUE(ldm::base::ToUtf8(L"").empty());
}

TEST(Utf, InvalidUtf8BecomesReplacementCharacter)
{
    const std::wstring utf16 = ldm::base::ToUtf16("a\xFF" "b");
    EXPECT_EQ(utf16, L"a�b");
}

TEST(Utf, LoneSurrogateBecomesReplacementCharacter)
{
    const std::wstring withLoneSurrogate{L'a', static_cast<wchar_t>(0xD800), L'b'};
    EXPECT_EQ(ldm::base::ToUtf8(withLoneSurrogate), "a\xEF\xBF\xBD" "b");
}

}  // namespace
