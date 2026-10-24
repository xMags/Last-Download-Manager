#include "platform/win/XmlLiteReader.h"
#include "ui/MediaCatalog.h"

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
using ldm::ui::MediaCatalog;
using ldm::ui::MediaItem;

std::string ReadFixture(const char* path)
{
    std::ifstream in(std::filesystem::path(LDM_MEDIA_FIXTURES) / path, std::ios::binary);
    EXPECT_TRUE(in) << path;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

class MediaCatalogTest : public ::testing::Test {
protected:
    MediaCatalogTest()
        : catalog_(MediaCatalog::Hooks{
              [this](ldm::core::FetchManifestCommand command) { fetches_.push_back(std::move(command)); },
              [this](ldm::ui::BrowserConnectionId connection, int tabId) { changes_.emplace_back(connection, tabId); },
              [](std::string_view xml) { return ldm::platform::ReadXml(xml); },
              [] { return std::chrono::system_clock::now(); }})
    {
    }

    static MediaFound Found(MediaKind kind, const std::string& url, int tabId = 5)
    {
        MediaFound media;
        media.tabId = tabId;
        media.kind = kind;
        media.url = url;
        media.pageUrl = "https://site.test/watch";
        media.pageTitle = "A film";
        media.userAgent = "UA";
        media.cookies = {{"cdn.test", true, "/", true, 0, "s", "1"}};
        return media;
    }

    void Answer(std::int64_t fetchId, std::string body, std::string finalUrl)
    {
        ldm::core::FetchFinished fetched;
        fetched.fetchId = fetchId;
        fetched.body = std::move(body);
        fetched.finalUrl = std::move(finalUrl);
        EXPECT_TRUE(catalog_.OnFetched(fetched));
    }

    MediaCatalog catalog_;
    std::vector<ldm::core::FetchManifestCommand> fetches_;
    std::vector<std::pair<ldm::ui::BrowserConnectionId, int>> changes_;
};

TEST_F(MediaCatalogTest, FilesAreOfferedAtOnce)
{
    auto media = Found(MediaKind::File, "https://cdn.test/clip.mp4");
    media.fileSize = 5'000'000;
    catalog_.OnMediaFound(1, media);
    EXPECT_TRUE(fetches_.empty());
    const auto* tab = catalog_.Find(1, 5);
    ASSERT_NE(tab, nullptr);
    EXPECT_EQ(tab->pageTitle, "A film");
    ASSERT_EQ(catalog_.Offered(*tab).size(), 1u);
    EXPECT_EQ(catalog_.Offered(*tab)[0]->fileSize, 5'000'000);
    EXPECT_TRUE(catalog_.HasMedia(*tab, false));
    EXPECT_EQ(changes_.size(), 1u);

    catalog_.OnMediaFound(1, media);
    EXPECT_EQ(catalog_.Find(1, 5)->items.size(), 1u) << "the same address once";
    EXPECT_EQ(catalog_.Find(2, 5), nullptr) << "tabs of another browser are apart";
}

TEST_F(MediaCatalogTest, AnHlsMasterIsFetchedReadAndItsLengthLookedUp)
{
    catalog_.OnMediaFound(1, Found(MediaKind::Hls, "https://cdn.test/master.m3u8"));
    ASSERT_EQ(fetches_.size(), 1u);
    EXPECT_EQ(fetches_[0].url, "https://cdn.test/master.m3u8");
    EXPECT_EQ(fetches_[0].referer, "https://site.test/watch");
    EXPECT_EQ(fetches_[0].userAgent, "UA");
    ASSERT_EQ(fetches_[0].cookies.size(), 1u);
    EXPECT_EQ(fetches_[0].maxBytes, ldm::media::kMaxManifestBytes);
    const auto* tab = catalog_.Find(1, 5);
    EXPECT_FALSE(catalog_.HasMedia(*tab, true)) << "nothing to offer while the manifest is on its way";

    Answer(fetches_[0].fetchId, ReadFixture("hls/master.m3u8"), "https://media.example.test/show/ep1/master.m3u8");
    tab = catalog_.Find(1, 5);
    ASSERT_EQ(catalog_.Offered(*tab).size(), 1u);
    const MediaItem& item = *catalog_.Offered(*tab)[0];
    EXPECT_EQ(item.finalUrl, "https://media.example.test/show/ep1/master.m3u8");
    EXPECT_FALSE(item.qualities.entries.empty());
    EXPECT_FALSE(item.seconds);

    ASSERT_EQ(fetches_.size(), 2u) << "the first media playlist, for the length";
    EXPECT_EQ(fetches_[1].url, "https://media.example.test/show/ep1/1080p/index.m3u8");
    Answer(fetches_[1].fetchId, ReadFixture("hls/vod-ts.m3u8"), fetches_[1].url);
    const MediaItem& sized = *catalog_.Offered(*catalog_.Find(1, 5))[0];
    EXPECT_DOUBLE_EQ(sized.seconds.value_or(0), 15.75);
    EXPECT_TRUE(sized.qualities.entries[0].approximateBytes);
}

TEST_F(MediaCatalogTest, MediaPlaylistsOfAListedMasterAreNotListedAgain)
{
    catalog_.OnMediaFound(1, Found(MediaKind::Hls, "https://media.example.test/show/ep1/master.m3u8"));
    Answer(fetches_[0].fetchId, ReadFixture("hls/master.m3u8"), "https://media.example.test/show/ep1/master.m3u8");
    // The player loads a variant's playlist too; the browser reports it.
    catalog_.OnMediaFound(1, Found(MediaKind::Hls, "https://media.example.test/show/ep1/1080p/index.m3u8"));
    const auto variantFetch = fetches_.back().fetchId;
    Answer(variantFetch, ReadFixture("hls/vod-ts.m3u8"), "https://media.example.test/show/ep1/1080p/index.m3u8");
    const auto* tab = catalog_.Find(1, 5);
    EXPECT_EQ(tab->items.size(), 2u);
    ASSERT_EQ(catalog_.Offered(*tab).size(), 1u);
    EXPECT_EQ(catalog_.Offered(*tab)[0]->url, "https://media.example.test/show/ep1/master.m3u8");
}

TEST_F(MediaCatalogTest, AMediaPlaylistAloneIsOneQuality)
{
    catalog_.OnMediaFound(1, Found(MediaKind::Hls, "https://media.example.test/show/ep1/index.m3u8"));
    Answer(fetches_[0].fetchId, ReadFixture("hls/vod-ts.m3u8"), "https://media.example.test/show/ep1/index.m3u8");
    const auto offered = catalog_.Offered(*catalog_.Find(1, 5));
    ASSERT_EQ(offered.size(), 1u);
    EXPECT_DOUBLE_EQ(offered[0]->seconds.value_or(0), 15.75);
    EXPECT_EQ(offered[0]->qualities.entries.size(), 1u);
}

TEST_F(MediaCatalogTest, DashManifestsAreReadWithTheXmlReader)
{
    catalog_.OnMediaFound(1, Found(MediaKind::Dash, "https://media.example.test/vod/movie/manifest.mpd"));
    Answer(fetches_[0].fetchId, ReadFixture("dash/template-number.mpd"), "https://media.example.test/vod/movie/manifest.mpd");
    const auto offered = catalog_.Offered(*catalog_.Find(1, 5));
    ASSERT_EQ(offered.size(), 1u);
    EXPECT_DOUBLE_EQ(offered[0]->seconds.value_or(0), 9.5);
    EXPECT_EQ(offered[0]->qualities.entries.size(), 5u);
}

TEST_F(MediaCatalogTest, ProtectedStreamsCountOnlyForTheNotice)
{
    catalog_.OnMediaFound(1, Found(MediaKind::Dash, "https://media.example.test/vod/movie/manifest.mpd"));
    Answer(fetches_[0].fetchId, ReadFixture("dash/drm.mpd"), "https://media.example.test/vod/movie/manifest.mpd");
    const auto* tab = catalog_.Find(1, 5);
    EXPECT_TRUE(catalog_.HasMedia(*tab, true));
    EXPECT_FALSE(catalog_.HasMedia(*tab, false));
}

TEST_F(MediaCatalogTest, FailedAndUnreadableManifestsAreNotOffered)
{
    catalog_.OnMediaFound(1, Found(MediaKind::Hls, "https://cdn.test/a.m3u8"));
    ldm::core::FetchFinished failed;
    failed.fetchId = fetches_[0].fetchId;
    failed.failure = ldm::core::TransferFailure{ldm::core::FailureReason::HttpError, ldm::core::FailureClass::Fatal, 403,
                                                "HTTP status 403"};
    EXPECT_TRUE(catalog_.OnFetched(failed));
    catalog_.OnMediaFound(1, Found(MediaKind::Hls, "https://cdn.test/b.m3u8"));
    Answer(fetches_[1].fetchId, "<html>not a playlist</html>", "https://cdn.test/b.m3u8");
    const auto* tab = catalog_.Find(1, 5);
    EXPECT_TRUE(catalog_.Offered(*tab).empty());
    EXPECT_EQ(tab->items[0].state, MediaItem::State::Failed);
    EXPECT_EQ(tab->items[0].failure, "HTTP status 403");
}

TEST_F(MediaCatalogTest, AnswersForAChangedPageAreDropped)
{
    catalog_.OnMediaFound(1, Found(MediaKind::Hls, "https://cdn.test/master.m3u8"));
    catalog_.OnReset(1, 5);
    EXPECT_EQ(catalog_.Find(1, 5), nullptr);
    Answer(fetches_[0].fetchId, ReadFixture("hls/master.m3u8"), "https://cdn.test/master.m3u8");
    EXPECT_EQ(catalog_.Find(1, 5), nullptr) << "a late answer does not bring the tab back";

    ldm::core::FetchFinished foreign;
    foreign.fetchId = 999;
    EXPECT_FALSE(catalog_.OnFetched(foreign)) << "not one of the catalog's fetches";
}

TEST_F(MediaCatalogTest, AClosedBrowserTakesItsTabs)
{
    catalog_.OnMediaFound(1, Found(MediaKind::File, "https://cdn.test/a.mp4", 5));
    catalog_.OnMediaFound(1, Found(MediaKind::File, "https://cdn.test/b.mp4", 6));
    catalog_.OnMediaFound(2, Found(MediaKind::File, "https://cdn.test/c.mp4", 5));
    catalog_.OnConnectionClosed(1);
    EXPECT_EQ(catalog_.Find(1, 5), nullptr);
    EXPECT_EQ(catalog_.Find(1, 6), nullptr);
    EXPECT_NE(catalog_.Find(2, 5), nullptr);
}

TEST_F(MediaCatalogTest, ATabListsAtMostItsLimit)
{
    for (size_t i = 0; i < MediaCatalog::kMaxItemsPerTab + 5; ++i) {
        catalog_.OnMediaFound(1, Found(MediaKind::File, "https://cdn.test/" + std::to_string(i) + ".mp4"));
    }
    EXPECT_EQ(catalog_.Find(1, 5)->items.size(), MediaCatalog::kMaxItemsPerTab);
}

}  // namespace
