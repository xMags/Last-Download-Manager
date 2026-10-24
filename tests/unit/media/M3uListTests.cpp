#include "media/M3uList.h"

#include <gtest/gtest.h>

namespace {

using ldm::media::ParseM3uLinks;

const char kBase[] = "https://music.test/lists/top.m3u";

TEST(M3uList, ReadsLinksWithTheirTitles)
{
    const auto entries = ParseM3uLinks(
        "\xEF\xBB\xBF#EXTM3U\r\n"
        "#EXTINF:215,Artist - Song\r\n"
        "https://cdn.music.test/a.mp3\r\n"
        "\r\n"
        "#EXTINF:-1 tvg-name=\"A, B\" group-title=\"x\",Radio, live\n"
        "  ../files/b.mp3  \n"
        "c.ogg\n",
        kBase);
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[0].url, "https://cdn.music.test/a.mp3");
    EXPECT_EQ(entries[0].title, "Artist - Song");
    EXPECT_EQ(entries[1].url, "https://music.test/files/b.mp3");
    EXPECT_EQ(entries[1].title, "Radio, live") << "a comma inside quotes is not the separator";
    EXPECT_EQ(entries[2].url, "https://music.test/lists/c.ogg");
    EXPECT_EQ(entries[2].title, "") << "a title belongs to the next entry only";
}

TEST(M3uList, AnHlsPlaylistIsNotAListOfLinks)
{
    EXPECT_TRUE(ParseM3uLinks("#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXTINF:4,\nseg1.ts\n", kBase).empty());
}

TEST(M3uList, LeavesOutOtherSchemesPathsAndRepeats)
{
    const auto entries = ParseM3uLinks("C:\\Music\\a.mp3\n"
                                       "file:///C:/b.mp3\n"
                                       "javascript:alert(1)\n"
                                       "ftp://files.test/c.mp3\n"
                                       "ftp://files.test/c.mp3\n"
                                       "https:///nohost.mp3\n"
                                       "d e.mp3\n",
                                       kBase);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].url, "ftp://files.test/c.mp3");
    EXPECT_EQ(entries[1].url, "https://music.test/lists/d%20e.mp3");
}

TEST(M3uList, TakesAtMostTheLimit)
{
    std::string text;
    for (size_t i = 0; i < ldm::media::kMaxM3uLinks + 5; ++i) {
        text += "f" + std::to_string(i) + ".mp3\n";
    }
    EXPECT_EQ(ParseM3uLinks(text, kBase).size(), ldm::media::kMaxM3uLinks);
}

}  // namespace
