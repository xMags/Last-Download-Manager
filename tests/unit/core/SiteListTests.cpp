#include "core/SiteList.h"

#include <gtest/gtest.h>

namespace {

using ldm::core::FindSiteVideo;

std::string Canonical(const char* page)
{
    const auto found = FindSiteVideo(page);
    return found ? found->canonicalUrl : std::string("(none)");
}

TEST(SiteList, YouTubeWatchPagesOfEveryFormGiveTheCanonicalAddress)
{
    const std::string expected = "https://www.youtube.com/watch?v=jNQXAC9IVRw";
    EXPECT_EQ(Canonical("https://www.youtube.com/watch?v=jNQXAC9IVRw"), expected);
    EXPECT_EQ(Canonical("https://youtube.com/watch?v=jNQXAC9IVRw"), expected);
    EXPECT_EQ(Canonical("https://m.youtube.com/watch?v=jNQXAC9IVRw"), expected);
    EXPECT_EQ(Canonical("http://www.youtube.com/watch?v=jNQXAC9IVRw"), expected);
    EXPECT_EQ(Canonical("https://WWW.YouTube.com/watch?v=jNQXAC9IVRw"), expected);
    EXPECT_EQ(Canonical("https://youtu.be/jNQXAC9IVRw"), expected);
    EXPECT_EQ(Canonical("https://youtu.be/jNQXAC9IVRw?si=abc&t=10"), expected);
    EXPECT_EQ(Canonical("https://www.youtube.com/shorts/jNQXAC9IVRw"), expected);
    EXPECT_EQ(Canonical("https://www.youtube.com/embed/jNQXAC9IVRw"), expected);
    EXPECT_EQ(Canonical("https://www.youtube-nocookie.com/embed/jNQXAC9IVRw"), expected);
    EXPECT_EQ(Canonical("https://www.youtube.com/live/jNQXAC9IVRw"), expected);
    EXPECT_EQ(Canonical("https://www.youtube.com/watch?v=jNQXAC9IVRw#t=30"), expected);
}

TEST(SiteList, OnlyTheVideoIdOfThePageIsKept)
{
    // A playlist, a start time and the order of parameters change nothing: one video is asked for.
    EXPECT_EQ(Canonical("https://www.youtube.com/watch?list=PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG&v=aqz-KE-bpKQ&t=42s"),
              "https://www.youtube.com/watch?v=aqz-KE-bpKQ");
    const auto page = FindSiteVideo("https://www.youtube.com/watch?v=aqz-KE-bpKQ&index=3");
    ASSERT_TRUE(page);
    EXPECT_EQ(page->videoId, "aqz-KE-bpKQ");
    EXPECT_EQ(page->site, ldm::core::VideoSite::YouTube);
}

TEST(SiteList, PagesThatAreNotOneVideoDoNotMatch)
{
    for (const char* page : {
             "https://www.youtube.com/",
             "https://www.youtube.com/playlist?list=PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG",
             "https://www.youtube.com/@Blender",
             "https://www.youtube.com/results?search_query=v%3DjNQXAC9IVRw",
             "https://www.youtube.com/watch",
             "https://www.youtube.com/watch?vv=jNQXAC9IVRw",
             "https://www.youtube.com/watch?v=",
             "https://www.youtube.com/watch?v=jNQXAC9IVR",    // 10 characters
             "https://www.youtube.com/watch?v=jNQXAC9IVRww",  // 12
             "https://www.youtube.com/watch?v=jNQXAC9IV%52w",
             "https://www.youtube.com/shorts/jNQXAC9IVRw/extra",
             "https://youtu.be/",
             "https://youtu.be/jNQXAC9IVRw/more",
             "https://www.youtube-nocookie.com/shorts/jNQXAC9IVRw",
             "https://music.youtube.com/watch?v=jNQXAC9IVRw",
             "https://youtube.com.example.com/watch?v=jNQXAC9IVRw",
             "https://example.com/watch?v=jNQXAC9IVRw",
             "https://user@www.youtube.com/watch?v=jNQXAC9IVRw",
             "https://www.youtube.com:8443/watch?v=jNQXAC9IVRw",
             "ftp://www.youtube.com/watch?v=jNQXAC9IVRw",
             "not an address",
         }) {
        EXPECT_FALSE(FindSiteVideo(page)) << page;
    }
}

TEST(SiteList, AnIdStartingWithADashStaysInsideTheBuiltAddress)
{
    // Ids may start with '-'; the resolver only ever sees the built address, never the bare id.
    const auto page = FindSiteVideo("https://www.youtube.com/watch?v=-oops--flag");
    ASSERT_TRUE(page);
    EXPECT_EQ(page->canonicalUrl, "https://www.youtube.com/watch?v=-oops--flag");
}

}  // namespace
