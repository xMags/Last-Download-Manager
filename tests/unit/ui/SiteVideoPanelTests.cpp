// Site videos in the media panel: the catalog's lookups per tab, the menu while one runs or after
// it failed, and the stream capture a quality gives.
#include "media/SiteVideo.h"
#include "ui/MediaCatalog.h"
#include "ui/MediaPanelMenu.h"
#include "ui/StreamChoice.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

using ldm::browser::MediaFound;
using ldm::browser::MediaKind;
using ldm::browser::SitePage;
using ldm::core::SiteLookupFinished;
using ldm::ui::MediaCatalog;
using ldm::ui::MediaItem;

std::string ReadFixture(const char* path)
{
    std::ifstream in(std::filesystem::path(LDM_MEDIA_FIXTURES) / path, std::ios::binary);
    EXPECT_TRUE(in) << path;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

ldm::media::SiteVideo Bunny()
{
    auto parsed = ldm::media::ParseSiteVideo(ReadFixture("site/youtube-4k.json"));
    EXPECT_TRUE(parsed.Ok());
    return parsed.Ok() ? std::move(parsed).Value() : ldm::media::SiteVideo{};
}

SitePage Page(const std::string& url, int tabId = 5)
{
    SitePage page;
    page.tabId = tabId;
    page.url = url;
    page.pageTitle = "Big Buck Bunny - YouTube";
    page.userAgent = "Browser/1";
    return page;
}

class SiteCatalogTest : public ::testing::Test {
protected:
    SiteCatalogTest()
        : catalog_(MediaCatalog::Hooks{
              [](ldm::core::FetchManifestCommand) {},
              [this](ldm::ui::BrowserConnectionId connection, int tabId) { changes_.emplace_back(connection, tabId); },
              nullptr,
              [] { return std::chrono::system_clock::now(); },
              [this](ldm::core::LookUpSiteVideoCommand command) { lookups_.push_back(std::move(command)); },
              [this](std::int64_t lookupId) { cancelled_.push_back(lookupId); }})
    {
    }

    const MediaItem* SiteItem(int tabId = 5) const
    {
        const auto* tab = catalog_.Find(1, tabId);
        if (tab == nullptr) {
            return nullptr;
        }
        const auto offered = catalog_.Offered(*tab, true);
        return offered.size() == 1 && offered[0]->siteVideo ? offered[0] : nullptr;
    }

    SiteLookupFinished Found(std::int64_t lookupId) const
    {
        SiteLookupFinished finished;
        finished.lookupId = lookupId;
        finished.video = Bunny();
        return finished;
    }

    MediaCatalog catalog_;
    std::vector<ldm::core::LookUpSiteVideoCommand> lookups_;
    std::vector<std::int64_t> cancelled_;
    std::vector<std::pair<ldm::ui::BrowserConnectionId, int>> changes_;
};

TEST_F(SiteCatalogTest, AVideoPageIsLookedUpAndOfferedWhileItIs)
{
    catalog_.OnSitePage(1, Page("https://www.youtube.com/watch?v=aqz-KE-bpKQ&t=12"));
    ASSERT_EQ(lookups_.size(), 1u);
    EXPECT_EQ(lookups_[0].pageUrl, "https://www.youtube.com/watch?v=aqz-KE-bpKQ");
    const MediaItem* item = SiteItem();
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->state, MediaItem::State::Fetching);
    EXPECT_TRUE(catalog_.HasMedia(*catalog_.Find(1, 5), false)) << "the panel shows at once";
    const auto menu = ldm::ui::BuildPanelMenu(catalog_.Offered(*catalog_.Find(1, 5), true), false);
    ASSERT_FALSE(menu.empty());
    EXPECT_EQ(menu[0].text, L"Finding qualities...");
    EXPECT_FALSE(menu[0].enabled);

    EXPECT_TRUE(catalog_.OnSiteLookedUp(Found(lookups_[0].lookupId)));
    item = SiteItem();
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->state, MediaItem::State::Ready);
    EXPECT_EQ(item->siteTitle, "Big Buck Bunny 60fps 4K - Official Blender Foundation Short Film");
    EXPECT_FALSE(item->qualities.entries.empty());
    EXPECT_GE(changes_.size(), 2u);
}

TEST_F(SiteCatalogTest, WhatThePlayerLoadsIsNotOfferedBesideTheVideo)
{
    MediaFound chunk;
    chunk.tabId = 5;
    chunk.kind = MediaKind::File;
    chunk.url = "https://rr1.example.test/videoplayback?itag=140&range=0-1000";
    chunk.pageUrl = "https://www.youtube.com/watch?v=aqz-KE-bpKQ";
    chunk.userAgent = "Browser/1";
    catalog_.OnMediaFound(1, chunk);  // before the page message
    catalog_.OnSitePage(1, Page("https://www.youtube.com/watch?v=aqz-KE-bpKQ"));
    chunk.url += "&rn=2";
    catalog_.OnMediaFound(1, chunk);  // after it
    EXPECT_EQ(catalog_.Find(1, 5)->items.size(), 1u);
    EXPECT_NE(SiteItem(), nullptr);
}

TEST_F(SiteCatalogTest, AnotherVideoInTheSameTabReplacesTheOfferAndTheOldAnswerIsDropped)
{
    catalog_.OnSitePage(1, Page("https://www.youtube.com/watch?v=aqz-KE-bpKQ"));
    catalog_.OnSitePage(1, Page("https://www.youtube.com/watch?v=aqz-KE-bpKQ"));
    EXPECT_EQ(lookups_.size(), 1u) << "the same video: no second lookup";
    catalog_.OnSitePage(1, Page("https://www.youtube.com/watch?v=jNQXAC9IVRw"));
    ASSERT_EQ(lookups_.size(), 2u);
    ASSERT_EQ(cancelled_.size(), 1u);
    EXPECT_EQ(cancelled_[0], lookups_[0].lookupId);

    // The core answers no cancelled lookup; one that came anyway is not the catalog's any more.
    EXPECT_FALSE(catalog_.OnSiteLookedUp(Found(lookups_[0].lookupId)));
    EXPECT_EQ(SiteItem()->state, MediaItem::State::Fetching) << "the new video is still being looked up";
    EXPECT_EQ(SiteItem()->url, "https://www.youtube.com/watch?v=jNQXAC9IVRw");
}

TEST_F(SiteCatalogTest, LeavingTheVideoOrLoadingAnotherPageEndsTheOffer)
{
    catalog_.OnSitePage(1, Page("https://www.youtube.com/watch?v=aqz-KE-bpKQ"));
    catalog_.OnSitePage(1, Page("https://www.youtube.com/feed/subscriptions"));
    EXPECT_EQ(SiteItem(), nullptr);
    EXPECT_FALSE(catalog_.HasMedia(*catalog_.Find(1, 5), true));
    ASSERT_EQ(cancelled_.size(), 1u);

    catalog_.OnSitePage(1, Page("https://www.youtube.com/watch?v=jNQXAC9IVRw"));
    catalog_.OnReset(1, 5);
    EXPECT_EQ(catalog_.Find(1, 5), nullptr);
    EXPECT_EQ(cancelled_.size(), 2u);

    catalog_.OnSitePage(1, Page("https://www.youtube.com/watch?v=jNQXAC9IVRw", 6));
    catalog_.OnConnectionClosed(1);
    EXPECT_EQ(cancelled_.size(), 3u);
}

TEST_F(SiteCatalogTest, AFailedLookupStaysOfferedWithItsReason)
{
    catalog_.OnSitePage(1, Page("https://www.youtube.com/watch?v=aqz-KE-bpKQ"));
    SiteLookupFinished failed;
    failed.lookupId = lookups_[0].lookupId;
    failed.problem = SiteLookupFinished::Problem::Video;
    failed.failure = "Private video. Sign in if you've been granted access to this video";
    EXPECT_TRUE(catalog_.OnSiteLookedUp(failed));
    const MediaItem* item = SiteItem();
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->state, MediaItem::State::Failed);
    const auto menu = ldm::ui::BuildPanelMenu(catalog_.Offered(*catalog_.Find(1, 5), true), false);
    EXPECT_EQ(menu[0].text, L"Private video. Sign in if you've been granted access to this video");
    EXPECT_FALSE(menu[0].enabled);
    EXPECT_FALSE(catalog_.OnSiteLookedUp(failed)) << "answered once";
}

TEST_F(SiteCatalogTest, PagesThatAreNotVideosStartNothing)
{
    catalog_.OnSitePage(1, Page("https://www.youtube.com/playlist?list=PL1"));
    catalog_.OnSitePage(1, Page("https://example.test/watch?v=aqz-KE-bpKQ"));
    EXPECT_TRUE(lookups_.empty());
    EXPECT_TRUE(changes_.empty());
}

TEST(SiteVideoCapture, AQualityGivesASiteVideoWithEachTracksFormatAndTheVideosTitle)
{
    ldm::ui::TabMedia tab;
    tab.pageUrl = "https://www.youtube.com/watch?v=aqz-KE-bpKQ&t=12";
    tab.pageTitle = "Big Buck Bunny - YouTube";
    tab.userAgent = "Browser/1";
    MediaItem item;
    item.key = 1;
    item.kind = MediaKind::Dash;
    item.url = "https://www.youtube.com/watch?v=aqz-KE-bpKQ";
    item.finalUrl = item.url;
    item.siteVideo = true;
    item.state = MediaItem::State::Ready;
    const auto video = Bunny();
    item.offer = video.offer;
    item.seconds = video.seconds;
    item.siteTitle = video.title;
    item.qualities = ldm::media::BuildQualityList(*item.offer, item.seconds);

    // The third entry is 1080p60 H.264 with the AAC track (see SiteVideoTests).
    const auto capture = ldm::ui::BuildStreamCapture(tab, item, ldm::ui::PanelAction{ldm::ui::PanelAction::Kind::DownloadStream, 1, 2});
    ASSERT_TRUE(capture) << capture.GetError().message;
    EXPECT_EQ(capture.Value().manifestUrl, "https://www.youtube.com/watch?v=aqz-KE-bpKQ");
    EXPECT_EQ(capture.Value().fileName, "Big Buck Bunny 60fps 4K - Official Blender Foundation Short Film.mp4");
    const auto& media = capture.Value().media;
    EXPECT_TRUE(media.siteVideo);
    EXPECT_EQ(media.kind, ldm::model::DownloadKind::Dash);
    ASSERT_EQ(media.tracks.size(), 2u);
    EXPECT_EQ(media.tracks[0].siteFormat, "299");
    EXPECT_EQ(media.tracks[0].expectedBytes, 257'619'653);
    EXPECT_EQ(media.tracks[1].siteFormat, "140");
    EXPECT_EQ(media.tracks[1].role, ldm::model::TrackRole::Audio);
    EXPECT_TRUE(capture.Value().cookies.empty()) << "no browser cookies for site videos";
}

}  // namespace
