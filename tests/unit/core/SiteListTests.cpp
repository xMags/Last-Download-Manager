#include "core/SiteList.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace {

using ldm::core::CheckSitePage;

// The address a listed site's video page is asked about; "(not listed)" for any other page.
std::string Listed(const std::string& page)
{
    const auto found = CheckSitePage(page);
    return found && found->automatic ? found->address : std::string("(not listed)");
}

TEST(SiteList, YouTubeWatchPagesOfEveryFormGiveTheCanonicalAddress)
{
    const std::string expected = "https://www.youtube.com/watch?v=jNQXAC9IVRw";
    EXPECT_EQ(Listed("https://www.youtube.com/watch?v=jNQXAC9IVRw"), expected);
    EXPECT_EQ(Listed("https://youtube.com/watch?v=jNQXAC9IVRw"), expected);
    EXPECT_EQ(Listed("https://m.youtube.com/watch?v=jNQXAC9IVRw"), expected);
    EXPECT_EQ(Listed("http://www.youtube.com/watch?v=jNQXAC9IVRw"), expected);
    EXPECT_EQ(Listed("https://WWW.YouTube.com/watch?v=jNQXAC9IVRw"), expected);
    EXPECT_EQ(Listed("https://youtu.be/jNQXAC9IVRw"), expected);
    EXPECT_EQ(Listed("https://youtu.be/jNQXAC9IVRw?si=abc&t=10"), expected);
    EXPECT_EQ(Listed("https://www.youtube.com/shorts/jNQXAC9IVRw"), expected);
    EXPECT_EQ(Listed("https://www.youtube.com/shorts/jNQXAC9IVRw/"), expected);
    EXPECT_EQ(Listed("https://www.youtube.com/embed/jNQXAC9IVRw"), expected);
    EXPECT_EQ(Listed("https://www.youtube-nocookie.com/embed/jNQXAC9IVRw"), expected);
    EXPECT_EQ(Listed("https://www.youtube.com/live/jNQXAC9IVRw"), expected);
    EXPECT_EQ(Listed("https://www.youtube.com/watch?v=jNQXAC9IVRw#t=30"), expected);
}

TEST(SiteList, OnlyTheVideoIdOfAYouTubePageIsKept)
{
    // A playlist, a start time and the order of parameters change nothing: one video is asked for.
    EXPECT_EQ(Listed("https://www.youtube.com/watch?list=PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG&v=aqz-KE-bpKQ&t=42s"),
              "https://www.youtube.com/watch?v=aqz-KE-bpKQ");
    // Ids may start with '-'; the resolver only ever sees the built address, never the bare id.
    EXPECT_EQ(Listed("https://www.youtube.com/watch?v=-oops--flag"), "https://www.youtube.com/watch?v=-oops--flag");
}

TEST(SiteList, OtherListedSitesGiveTheirVideoPagesWithoutWhatThePageAdded)
{
    EXPECT_EQ(Listed("https://www.dailymotion.com/video/x5kesuj?playlist=x6hynp"), "https://www.dailymotion.com/video/x5kesuj");
    EXPECT_EQ(Listed("https://dai.ly/x5kesuj"), "https://www.dailymotion.com/video/x5kesuj");
    EXPECT_EQ(Listed("https://vimeo.com/76979871"), "https://vimeo.com/76979871");
    EXPECT_EQ(Listed("https://www.twitch.tv/videos/6528877?t=1h"), "https://www.twitch.tv/videos/6528877");
    EXPECT_EQ(Listed("https://www.twitch.tv/riotgames/clip/AbcDef-123"), "https://www.twitch.tv/riotgames/clip/AbcDef-123");
    EXPECT_EQ(Listed("https://clips.twitch.tv/AbcDef-123"), "https://clips.twitch.tv/AbcDef-123");
    EXPECT_EQ(Listed("https://www.ted.com/talks/dan_dennett_on_our_consciousness"),
              "https://www.ted.com/talks/dan_dennett_on_our_consciousness");
    EXPECT_EQ(Listed("https://www.instagram.com/reels/C9x5pUZvXmD/"), "https://www.instagram.com/reel/C9x5pUZvXmD/");
}

TEST(SiteList, PagesThatAreNotCertainlyOneVideoAreNotListed)
{
    for (const char* page : {
             "https://www.youtube.com/",
             "https://www.youtube.com/playlist?list=PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG",
             "https://www.youtube.com/@Blender",
             "https://www.youtube.com/results?search_query=v%3DjNQXAC9IVRw",
             "https://www.youtube.com/watch",
             "https://www.youtube.com/watch/",
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
             "https://www.youtube.com:8443/watch?v=jNQXAC9IVRw",
             "https://www.dailymotion.com/",
             "https://www.dailymotion.com/video/",
             "https://vimeo.com/channels/staffpicks",
             "https://www.twitch.tv/riotgames",
             "https://www.twitch.tv/videos/latest",
             "https://soundcloud.com/ethmusic/lostin-powers-she-so-heavy",  // sound only: no player to sit on
             "https://www.ted.com/talks",
             "https://artist.bandcamp.com/track/a-song",
             "https://www.instagram.com/p/C9x5pUZvXmD/",
             "https://www.reddit.com/r/videos/comments/6rrwyj/that_small_heart_attack/",
             "https://x.com/NASA/status/1844412155573170387",
         }) {
        EXPECT_EQ(Listed(page), "(not listed)") << page;
    }
}

TEST(SiteList, MoreSitesVideoPagesGiveTheAddressTheResolverKnows)
{
    EXPECT_EQ(Listed("https://www.dailymotion.com/embed/video/x5kesuj"), "https://www.dailymotion.com/video/x5kesuj");
    EXPECT_EQ(Listed("https://www.facebook.com/watch/?v=274175099429670&ref=sharing"),
              "https://www.facebook.com/watch/?v=274175099429670");
    EXPECT_EQ(Listed("https://m.facebook.com/video.php?v=274175099429670"), "https://www.facebook.com/watch/?v=274175099429670");
    EXPECT_EQ(Listed("https://www.facebook.com/NASA/videos/274175099429670/"), "https://www.facebook.com/watch/?v=274175099429670");
    EXPECT_EQ(Listed("https://www.facebook.com/NASA/videos/launch-day/274175099429670"),
              "https://www.facebook.com/watch/?v=274175099429670");
    EXPECT_EQ(Listed("https://www.facebook.com/reel/1195289147628387"), "https://www.facebook.com/reel/1195289147628387");
    EXPECT_EQ(Listed("https://fb.watch/aBcD12-xY/"), "https://fb.watch/aBcD12-xY/");
    EXPECT_EQ(Listed("https://rumble.com/v5pv5f-the-title.html?mref=x"), "https://rumble.com/v5pv5f-the-title.html");
    EXPECT_EQ(Listed("https://rumble.com/embed/v5pv5f/"), "https://rumble.com/embed/v5pv5f");
    EXPECT_EQ(Listed("https://kick.com/xqc/videos/0b5c8a6a-6b4e-4b8a-9f61-8b6a2e1d3c4f"),
              "https://kick.com/xqc/videos/0b5c8a6a-6b4e-4b8a-9f61-8b6a2e1d3c4f");
    EXPECT_EQ(Listed("https://kick.com/mxddy?clip=clip_01GYXVB5Y8PWAPWCWMSBCFB05X"),
              "https://kick.com/mxddy/clips/clip_01GYXVB5Y8PWAPWCWMSBCFB05X");
    EXPECT_EQ(Listed("https://kick.com/mxddy/clips/clip_01GYXVB5Y8PWAPWCWMSBCFB05X"),
              "https://kick.com/mxddy/clips/clip_01GYXVB5Y8PWAPWCWMSBCFB05X");
    EXPECT_EQ(Listed("https://vk.com/video-77521_162222515?list=abc"), "https://vk.com/video-77521_162222515");
    EXPECT_EQ(Listed("https://m.vk.com/clip-23538238_456239151"), "https://vk.com/clip-23538238_456239151");
    EXPECT_EQ(Listed("https://vkvideo.ru/video1234_5678"), "https://vkvideo.ru/video1234_5678");
    EXPECT_EQ(Listed("https://odysee.com/@Mantega:1/First-day-LBRY:1"), "https://odysee.com/@Mantega:1/First-day-LBRY:1");
    EXPECT_EQ(Listed("https://odysee.com/First-day-LBRY:1"), "https://odysee.com/First-day-LBRY:1");
    EXPECT_EQ(Listed("https://www.bitchute.com/video/Yti_j9A-UZ4/"), "https://www.bitchute.com/video/Yti_j9A-UZ4/");
    EXPECT_EQ(Listed("https://old.bitchute.com/embed/Yti_j9A-UZ4"), "https://www.bitchute.com/video/Yti_j9A-UZ4/");
    EXPECT_EQ(Listed("https://www.nicovideo.jp/watch/sm9?ref=top"), "https://www.nicovideo.jp/watch/sm9");
    EXPECT_EQ(Listed("https://sp.nicovideo.jp/watch/1173108780"), "https://www.nicovideo.jp/watch/1173108780");
    EXPECT_EQ(Listed("https://nico.ms/sm9"), "https://www.nicovideo.jp/watch/sm9");
    EXPECT_EQ(Listed("https://ok.ru/video/1484130554189"), "https://ok.ru/video/1484130554189");
    EXPECT_EQ(Listed("https://m.ok.ru/videoembed/1484130554189"), "https://ok.ru/video/1484130554189");
    EXPECT_EQ(Listed("http://coub.com/view/5u5n1"), "https://coub.com/view/5u5n1");
    EXPECT_EQ(Listed("https://www.loom.com/share/43d05f362f734614a2e81b4694a3a523?sid=1"),
              "https://www.loom.com/share/43d05f362f734614a2e81b4694a3a523");
}

TEST(SiteList, MoreSitesPagesThatAreNotOneVideoAreNotListed)
{
    for (const char* page : {
             "https://www.facebook.com/",
             "https://www.facebook.com/watch/",
             "https://www.facebook.com/watch/?v=abc",
             "https://www.facebook.com/NASA",
             "https://www.facebook.com/NASA/videos/",
             "https://www.facebook.com/NASA/posts/274175099429670",
             "https://www.facebook.com/groups/123/permalink/456/",
             "https://rumble.com/videos?sort=views",
             "https://rumble.com/c/channel",
             "https://rumble.com/vabout.html",
             "https://kick.com/xqc",  // the channel: live only at times
             "https://kick.com/xqc/videos",
             "https://kick.com/xqc?clip=other",
             "https://vk.com/bs.official?w=wall-23538238_35",
             "https://vk.com/video",
             "https://vk.com/video-77521",
             "https://vk.com/video-77521_x",
             "https://odysee.com/@Mantega:1",
             "https://odysee.com/$/trending",
             "https://www.bitchute.com/channel/abc/",
             "https://www.nicovideo.jp/watch/",
             "https://www.nicovideo.jp/watch/sm",
             "https://ok.ru/video/",
             "https://ok.ru/group/123",
             "https://coub.com/community/animals",
             "https://www.loom.com/share/folder/43d05f362f734614a2e81b4694a3a523",
             "https://www.loom.com/share/43d05f362f734614a2e81b4694a3a52",
             "https://streamable.com/dnd1",  // its servers refused LDM in the live test: right-click only
             "https://www.bilibili.com/video/BV1ay4y1d77f/",  // yt-dlp could not read its pages in the live test
         }) {
        EXPECT_EQ(Listed(page), "(not listed)") << page;
    }
}

TEST(SiteList, AnyPublicPageMayBeAskedAboutAsItStands)
{
    const auto page = CheckSitePage("  https://www.reddit.com/r/videos/comments/6rrwyj/x/?share=1#comments ");
    ASSERT_TRUE(page);
    EXPECT_FALSE(page->automatic);
    EXPECT_EQ(page->address, "https://www.reddit.com/r/videos/comments/6rrwyj/x/?share=1");
    const auto playlist = CheckSitePage("https://www.youtube.com/playlist?list=PL1");
    ASSERT_TRUE(playlist);
    EXPECT_EQ(playlist->address, "https://www.youtube.com/playlist?list=PL1");
    EXPECT_TRUE(CheckSitePage("https://example.test:8080/video/1"));
}

TEST(SiteList, AddressesTheResolverMustNeverSeeAreRefused)
{
    for (const char* page : {
             "ftp://www.youtube.com/watch?v=jNQXAC9IVRw",
             "file:///C:/Windows/win.ini",
             "javascript:alert(1)",
             "https://user@www.youtube.com/watch?v=jNQXAC9IVRw",
             "https://user:secret@example.test/",
             "http://localhost/video",
             "http://127.0.0.1:8080/",
             "http://192.168.1.10/cam.m3u8",
             "http://[::1]/",
             "http://router/",
             "https://example.test/a b",
             "https://example.test/\x01",
             "--exec calc https://example.test/",
             "not an address",
             "",
         }) {
        EXPECT_FALSE(CheckSitePage(page)) << page;
    }
    EXPECT_FALSE(CheckSitePage("https://example.test/" + std::string(ldm::core::kMaxSitePageBytes, 'a')));
}

TEST(SiteList, TheExtensionIsToldTheListedSitesHosts)
{
    const auto hosts = ldm::core::AutomaticSiteHosts();
    for (const char* host : {"youtube.com", "youtu.be", "dailymotion.com", "twitch.tv", "instagram.com", "facebook.com",
                             "fb.watch", "vkvideo.ru", "nico.ms"}) {
        EXPECT_NE(std::find(hosts.begin(), hosts.end(), host), hosts.end()) << host;
    }
    for (const char* host : {"reddit.com", "soundcloud.com", "bandcamp.com"}) {
        EXPECT_EQ(std::find(hosts.begin(), hosts.end(), host), hosts.end()) << host;
    }
}

TEST(SiteList, SignInIsOnlyForTheSitesTheUserChecked)
{
    using ldm::core::UsesSignIn;
    EXPECT_TRUE(UsesSignIn("vimeo youtube", "https://vimeo.com/76979871"));
    EXPECT_TRUE(UsesSignIn("vimeo youtube", "https://m.youtube.com/watch?v=jNQXAC9IVRw"));
    EXPECT_TRUE(UsesSignIn("youtube", "https://youtu.be/jNQXAC9IVRw"));
    EXPECT_FALSE(UsesSignIn("vimeo", "https://www.youtube.com/watch?v=jNQXAC9IVRw"));
    EXPECT_FALSE(UsesSignIn("vimeo", "https://notvimeo.com/1"));
    EXPECT_FALSE(UsesSignIn("vimeo", "https://vimeo.com.example.test/1"));
    EXPECT_FALSE(UsesSignIn("", "https://vimeo.com/1"));
    EXPECT_FALSE(UsesSignIn("unknown", "https://vimeo.com/1"));
    EXPECT_EQ(ldm::core::SignInHosts("x reddit"), (std::vector<std::string>{"x.com", "twitter.com", "reddit.com"}));
    const auto& sites = ldm::core::SignInSites();
    const auto youtube = std::find_if(sites.begin(), sites.end(), [](const auto& s) { return s.key == "youtube"; });
    ASSERT_NE(youtube, sites.end());
    EXPECT_TRUE(youtube->flagsAccounts);
}

}  // namespace
