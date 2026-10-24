#include "ui/MediaPanelMenu.h"

#include "media/QualityList.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using ldm::ui::BuildPanelMenu;
using ldm::ui::MediaItem;
using ldm::ui::PanelAction;
using ldm::ui::PanelMenuItem;

MediaItem Stream(std::int64_t key)
{
    MediaItem item;
    item.key = key;
    item.kind = ldm::browser::MediaKind::Dash;
    item.state = MediaItem::State::Ready;
    item.offer = ldm::media::MediaOffer{};
    ldm::media::QualityEntry video;
    video.kind = ldm::media::QualityKind::Video;
    video.height = 1080;
    video.videoCodec = "H.264";
    video.approximateBytes = 3 * 1024 * 1024;
    ldm::media::QualityEntry pictureOnly = video;
    pictureOnly.kind = ldm::media::QualityKind::VideoOnly;
    ldm::media::QualityEntry sound;
    sound.kind = ldm::media::QualityKind::AudioOnly;
    sound.audioCodec = "AAC";
    sound.bitsPerSecond = 128'000;
    item.qualities.entries = {video, pictureOnly, sound};
    return item;
}

const PanelMenuItem* Named(const std::vector<PanelMenuItem>& menu, const std::wstring& text)
{
    for (const PanelMenuItem& item : menu) {
        if (item.text == text) {
            return &item;
        }
    }
    return nullptr;
}

TEST(MediaPanelMenu, OneStreamListsItsQualitiesWithSubmenusForTheRest)
{
    const MediaItem stream = Stream(7);
    const auto menu = BuildPanelMenu({&stream}, false);
    ASSERT_GE(menu.size(), 5u);
    EXPECT_EQ(menu[0].text.substr(0, 11), L"1080p H.264");
    EXPECT_NE(menu[0].text.find(L"\t~"), std::wstring::npos) << "the approximate size on the right";
    ASSERT_TRUE(menu[0].action);
    EXPECT_EQ(menu[0].action->kind, PanelAction::Kind::DownloadStream);
    EXPECT_EQ(menu[0].action->itemKey, 7);
    EXPECT_EQ(menu[0].action->entry, 0u);
    const PanelMenuItem* videoOnly = Named(menu, L"Video only");
    ASSERT_NE(videoOnly, nullptr);
    ASSERT_EQ(videoOnly->children.size(), 1u);
    EXPECT_EQ(videoOnly->children[0].action->entry, 1u);
    const PanelMenuItem* audioOnly = Named(menu, L"Audio only");
    ASSERT_NE(audioOnly, nullptr);
    EXPECT_EQ(audioOnly->children[0].text, L"Audio AAC 128 kbps");
    EXPECT_NE(Named(menu, L"Make the panel smaller"), nullptr);
    EXPECT_NE(Named(menu, L"Hide the panel for this page"), nullptr);
}

TEST(MediaPanelMenu, SeveralStreamsGetOneSubmenuEachAndFilesOneLine)
{
    const MediaItem first = Stream(1);
    const MediaItem second = Stream(2);
    MediaItem file;
    file.key = 3;
    file.kind = ldm::browser::MediaKind::File;
    file.url = "https://cdn.test/videos/clip.mp4?x=1";
    file.fileSize = 5 * 1024 * 1024;
    file.state = MediaItem::State::Ready;
    const auto menu = BuildPanelMenu({&first, &second, &file}, true);
    EXPECT_EQ(menu[0].text, L"DASH stream: 1080p H.264");
    EXPECT_FALSE(menu[0].children.empty());
    EXPECT_EQ(menu[1].children[0].action->itemKey, 2);
    EXPECT_EQ(menu[2].text.substr(0, 17), L"Download MP4 file");
    EXPECT_EQ(menu[2].action->kind, PanelAction::Kind::DownloadFile);
    EXPECT_NE(Named(menu, L"Make the panel bigger"), nullptr) << "in mini mode";
}

TEST(MediaPanelMenu, LiveQualitiesSayTheyRecordAndLiveDashIsANotice)
{
    MediaItem stream = Stream(7);
    stream.kind = ldm::browser::MediaKind::Hls;
    stream.offer->live = true;
    const auto menu = BuildPanelMenu({&stream}, false);
    ASSERT_FALSE(menu.empty());
    EXPECT_EQ(menu[0].text, L"1080p H.264 (live)") << "no size: a recording grows while it runs";
    EXPECT_TRUE(menu[0].action);

    stream.kind = ldm::browser::MediaKind::Dash;
    const auto dash = BuildPanelMenu({&stream}, false);
    ASSERT_FALSE(dash.empty());
    EXPECT_FALSE(dash[0].enabled);
    EXPECT_FALSE(dash[0].action);
    EXPECT_NE(dash[0].text.find(L"live DASH"), std::wstring::npos);
}

TEST(MediaPanelMenu, StreamSubmenusSayWhenTheyAreProtectedOrLive)
{
    MediaItem clear = Stream(1);
    MediaItem live = Stream(2);
    live.kind = ldm::browser::MediaKind::Hls;
    live.offer->live = true;
    MediaItem protectedStream = Stream(3);
    protectedStream.qualities.entries.clear();
    protectedStream.qualities.drm = ldm::media::DrmInfo{{"Widevine"}};
    const auto menu = BuildPanelMenu({&clear, &live, &protectedStream}, false);
    ASSERT_GE(menu.size(), 3u);
    EXPECT_EQ(menu[0].text, L"DASH stream: 1080p H.264");
    EXPECT_EQ(menu[1].text, L"HLS stream: 1080p H.264 (live)");
    EXPECT_EQ(menu[2].text, L"DASH stream (protected)");
    ASSERT_EQ(menu[2].children.size(), 1u);
    EXPECT_FALSE(menu[2].children[0].enabled);
}

TEST(MediaPanelMenu, AProtectedStreamIsANoticeOnly)
{
    MediaItem stream = Stream(4);
    stream.qualities.entries.clear();
    stream.qualities.drm = ldm::media::DrmInfo{{"Widevine", "PlayReady"}};
    const auto menu = BuildPanelMenu({&stream}, false);
    EXPECT_EQ(menu[0].text, L"This video is protected (Widevine, PlayReady) and cannot be downloaded");
    EXPECT_FALSE(menu[0].enabled);
    EXPECT_FALSE(menu[0].action);
}

}  // namespace
