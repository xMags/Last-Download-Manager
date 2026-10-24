#include "ui/StreamChoice.h"

#include "media/QualityList.h"

#include <gtest/gtest.h>

namespace {

using ldm::media::MediaOffer;
using ldm::media::PlaylistAddress;
using ldm::media::Rendition;
using ldm::media::Variant;
using ldm::model::TrackRole;
using ldm::ui::MediaItem;
using ldm::ui::PanelAction;
using ldm::ui::TabMedia;

TabMedia Tab()
{
    TabMedia tab;
    tab.pageUrl = "https://site.test/watch?v=1";
    tab.pageTitle = "My: Video";
    tab.userAgent = "Browser/1";
    return tab;
}

// An HLS master playlist: two pictures without sound, an English and a German audio track, and
// subtitles.
MediaItem HlsItem()
{
    MediaOffer offer;
    offer.format = ldm::media::StreamFormat::Hls;
    for (const int height : {720, 1080}) {
        Variant variant;
        variant.bandwidth = static_cast<std::uint64_t>(height) * 3000;
        variant.resolution = ldm::media::Resolution{height * 16 / 9, height};
        variant.codecs = "avc1.64001f,mp4a.40.2";
        variant.audioGroup = "aud";
        variant.subtitleGroup = "subs";
        variant.source = PlaylistAddress{"https://cdn.test/v" + std::to_string(height) + ".m3u8"};
        offer.variants.push_back(variant);
    }
    for (const char* language : {"en", "de"}) {
        Rendition audio;
        audio.groupId = "aud";
        audio.language = language;
        audio.name = std::string("Audio ") + language;
        audio.isDefault = std::string(language) == "en";
        audio.codecs = "mp4a.40.2";
        audio.source = PlaylistAddress{std::string("https://cdn.test/a-") + language + ".m3u8"};
        offer.audio.push_back(audio);
    }
    Rendition subtitles;
    subtitles.groupId = "subs";
    subtitles.language = "fr";
    subtitles.source = PlaylistAddress{"https://cdn.test/subs-fr.m3u8"};
    offer.subtitles.push_back(subtitles);

    MediaItem item;
    item.key = 3;
    item.kind = ldm::browser::MediaKind::Hls;
    item.url = "https://cdn.test/master.m3u8";
    item.finalUrl = "https://cdn.test/master.m3u8?signed=1";
    item.state = MediaItem::State::Ready;
    item.offer = offer;
    item.seconds = 60;
    item.qualities = ldm::media::BuildQualityList(offer, item.seconds);
    return item;
}

size_t EntryOf(const MediaItem& item, ldm::media::QualityKind kind)
{
    for (size_t i = 0; i < item.qualities.entries.size(); ++i) {
        if (item.qualities.entries[i].kind == kind) {
            return i;
        }
    }
    ADD_FAILURE() << "no entry of that kind";
    return 0;
}

TEST(StreamChoice, AVideoQualityTakesItsPictureAndTheDefaultSound)
{
    const MediaItem item = HlsItem();
    const size_t entry = EntryOf(item, ldm::media::QualityKind::Video);  // the best picture comes first
    const auto capture = ldm::ui::BuildStreamCapture(Tab(), item, PanelAction{PanelAction::Kind::DownloadStream, 3, entry});
    ASSERT_TRUE(capture) << capture.GetError().message;
    EXPECT_EQ(capture.Value().manifestUrl, "https://cdn.test/master.m3u8?signed=1");
    EXPECT_EQ(capture.Value().fileName, "My_ Video.mp4");
    EXPECT_EQ(capture.Value().pageUrl, "https://site.test/watch?v=1");
    const auto& media = capture.Value().media;
    EXPECT_EQ(media.kind, ldm::model::DownloadKind::Hls);
    ASSERT_EQ(media.tracks.size(), 2u);
    EXPECT_EQ(media.tracks[0].role, TrackRole::Video);
    EXPECT_EQ(media.tracks[0].sourceUrl, "https://cdn.test/v1080.m3u8");
    EXPECT_FALSE(media.tracks[0].segments) << "an HLS playlist is read when the download starts";
    EXPECT_EQ(media.tracks[1].role, TrackRole::Audio);
    EXPECT_EQ(media.tracks[1].sourceUrl, "https://cdn.test/a-en.m3u8");
    EXPECT_EQ(media.tracks[1].language, "en");
    EXPECT_EQ(media.approximateBytes, item.qualities.entries[entry].approximateBytes);
}

TEST(StreamChoice, VideoOnlyAndAudioOnlyTakeOneTrack)
{
    const MediaItem item = HlsItem();
    const auto videoOnly = ldm::ui::BuildStreamCapture(
        Tab(), item, PanelAction{PanelAction::Kind::DownloadStream, 3, EntryOf(item, ldm::media::QualityKind::VideoOnly)});
    ASSERT_TRUE(videoOnly);
    ASSERT_EQ(videoOnly.Value().media.tracks.size(), 1u);
    EXPECT_EQ(videoOnly.Value().media.tracks[0].role, TrackRole::Video);

    const auto audioOnly = ldm::ui::BuildStreamCapture(
        Tab(), item, PanelAction{PanelAction::Kind::DownloadStream, 3, EntryOf(item, ldm::media::QualityKind::AudioOnly)});
    ASSERT_TRUE(audioOnly);
    ASSERT_EQ(audioOnly.Value().media.tracks.size(), 1u);
    EXPECT_EQ(audioOnly.Value().media.tracks[0].role, TrackRole::Audio);
    EXPECT_EQ(audioOnly.Value().fileName, "My_ Video.m4a");
}

TEST(StreamChoice, SubtitlesAreADownloadOfTheirOwn)
{
    const MediaItem item = HlsItem();
    const auto capture = ldm::ui::BuildStreamCapture(Tab(), item, PanelAction{PanelAction::Kind::DownloadSubtitles, 3, 0});
    ASSERT_TRUE(capture);
    ASSERT_EQ(capture.Value().media.tracks.size(), 1u);
    EXPECT_EQ(capture.Value().media.tracks[0].role, TrackRole::Subtitles);
    EXPECT_EQ(capture.Value().media.tracks[0].sourceUrl, "https://cdn.test/subs-fr.m3u8");
    EXPECT_EQ(capture.Value().fileName, "My_ Video.fr.vtt");
}

TEST(StreamChoice, DashTracksCarryTheirListsAndTheManifestAddress)
{
    MediaItem item = HlsItem();
    item.kind = ldm::browser::MediaKind::Dash;
    ldm::media::TrackSegments segments;
    ldm::media::Segment segment;
    segment.url = "https://cdn.test/v/1.m4s";
    segment.seconds = 2;
    segments.segments.push_back(segment);
    for (Variant& variant : item.offer->variants) {
        variant.source = segments;
    }
    for (Rendition& audio : item.offer->audio) {
        audio.source = segments;
    }
    item.fromPrivateNetwork = true;
    const auto capture = ldm::ui::BuildStreamCapture(
        Tab(), item, PanelAction{PanelAction::Kind::DownloadStream, 3, EntryOf(item, ldm::media::QualityKind::Video)});
    ASSERT_TRUE(capture);
    EXPECT_EQ(capture.Value().media.kind, ldm::model::DownloadKind::Dash);
    EXPECT_TRUE(capture.Value().media.fromPrivateNetwork);
    for (const auto& track : capture.Value().media.tracks) {
        EXPECT_EQ(track.sourceUrl, "https://cdn.test/master.m3u8?signed=1");
        ASSERT_TRUE(track.segments);
        EXPECT_EQ(track.segments->segments.size(), 1u);
    }
}

TEST(StreamChoice, LiveHlsIsRecordedWithoutASizeAndLiveDashIsRefused)
{
    MediaItem live = HlsItem();
    live.offer->live = true;
    const auto recorded = ldm::ui::BuildStreamCapture(Tab(), live, PanelAction{PanelAction::Kind::DownloadStream, 3, 0});
    ASSERT_TRUE(recorded) << recorded.GetError().message;
    EXPECT_EQ(recorded.Value().media.kind, ldm::model::DownloadKind::Hls);
    EXPECT_FALSE(recorded.Value().media.approximateBytes) << "its size depends on how long it runs";
    EXPECT_EQ(recorded.Value().fileName, "My_ Video.mp4");

    live.kind = ldm::browser::MediaKind::Dash;
    const auto refused = ldm::ui::BuildStreamCapture(Tab(), live, PanelAction{PanelAction::Kind::DownloadStream, 3, 0});
    ASSERT_FALSE(refused);
    EXPECT_NE(refused.GetError().message.find("live DASH"), std::string::npos);
}

TEST(StreamChoice, StaleEntriesAreRefused)
{

    const MediaItem item = HlsItem();
    EXPECT_FALSE(ldm::ui::BuildStreamCapture(Tab(), item, PanelAction{PanelAction::Kind::DownloadStream, 3, 99}));
    EXPECT_FALSE(ldm::ui::BuildStreamCapture(Tab(), item, PanelAction{PanelAction::Kind::DownloadSubtitles, 3, 5}));
}

}  // namespace
