#include "core/QualityTracks.h"

#include <gtest/gtest.h>

#include <optional>
#include <vector>

namespace {

using ldm::core::PickSiteQuality;
using ldm::media::QualityEntry;
using ldm::media::QualityKind;
using ldm::model::SiteQuality;

QualityEntry Entry(QualityKind kind, std::optional<int> height = std::nullopt)
{
    QualityEntry entry;
    entry.kind = kind;
    entry.height = height;
    return entry;
}

// A list as the quality list gives it: pictures highest first, then video only, then sound only.
ldm::media::QualityList List()
{
    ldm::media::QualityList list;
    list.entries = {Entry(QualityKind::Video, 2160), Entry(QualityKind::Video, 1080), Entry(QualityKind::Video, 720),
                    Entry(QualityKind::Video, 360), Entry(QualityKind::VideoOnly, 2160), Entry(QualityKind::AudioOnly),
                    Entry(QualityKind::AudioOnly)};
    return list;
}

TEST(QualityTracks, TheRulePicksTheBestPictureWithinItsHeight)
{
    const auto list = List();
    EXPECT_EQ(PickSiteQuality(list, SiteQuality::Best), 0u);
    EXPECT_EQ(PickSiteQuality(list, SiteQuality::UpTo2160), 0u);
    EXPECT_EQ(PickSiteQuality(list, SiteQuality::UpTo1440), 1u);
    EXPECT_EQ(PickSiteQuality(list, SiteQuality::UpTo1080), 1u);
    EXPECT_EQ(PickSiteQuality(list, SiteQuality::UpTo720), 2u);
    EXPECT_EQ(PickSiteQuality(list, SiteQuality::UpTo480), 3u);
    EXPECT_EQ(PickSiteQuality(list, SiteQuality::AudioOnly), 5u);
}

TEST(QualityTracks, EveryPictureTooHighGivesTheLowestAndSoundOnlyPagesGiveTheirSound)
{
    ldm::media::QualityList high;
    high.entries = {Entry(QualityKind::Video, 2160), Entry(QualityKind::Video, 1440), Entry(QualityKind::AudioOnly)};
    EXPECT_EQ(PickSiteQuality(high, SiteQuality::UpTo480), 1u);

    ldm::media::QualityList sound;
    sound.entries = {Entry(QualityKind::AudioOnly)};
    EXPECT_EQ(PickSiteQuality(sound, SiteQuality::Best), 0u);
    EXPECT_EQ(PickSiteQuality(sound, SiteQuality::UpTo720), 0u);

    ldm::media::QualityList pictures;
    pictures.entries = {Entry(QualityKind::Video, 720)};
    EXPECT_FALSE(PickSiteQuality(pictures, SiteQuality::AudioOnly)) << "no sound on its own";
    EXPECT_FALSE(PickSiteQuality(ldm::media::QualityList{}, SiteQuality::Best));
}

TEST(QualityTracks, AQualityWithSeparateSoundGivesBothTracksWithTheirFormats)
{
    ldm::media::MediaOffer offer;
    ldm::media::Variant variant;
    variant.codecs = "avc1.640028";
    variant.source = ldm::media::PlaylistAddress{"https://v.example.test/1080.m3u8"};
    variant.site = ldm::media::SiteFormat{"v1080", std::nullopt, "https://www.example.test/", "mp4"};
    offer.variants.push_back(variant);
    ldm::media::Rendition audio;
    audio.source = ldm::media::PlaylistAddress{"https://v.example.test/a.m3u8"};
    audio.site = ldm::media::SiteFormat{"a1", std::nullopt};
    offer.audio.push_back(audio);
    QualityEntry entry = Entry(QualityKind::Video, 1080);
    entry.variant = 0;
    entry.audio = 0;

    const auto tracks = ldm::core::TracksForQuality(offer, entry, "https://www.example.test/watch/1");
    ASSERT_TRUE(tracks.Ok()) << tracks.GetError().message;
    ASSERT_EQ(tracks.Value().tracks.size(), 2u);
    EXPECT_TRUE(tracks.Value().picture);
    EXPECT_EQ(tracks.Value().referer, "https://www.example.test/");
    EXPECT_EQ(tracks.Value().tracks[0].siteFormat, "v1080");
    EXPECT_EQ(tracks.Value().tracks[1].siteFormat, "a1");
    EXPECT_EQ(tracks.Value().tracks[1].role, ldm::model::TrackRole::Audio);
    EXPECT_EQ(ldm::core::SiteDownloadKind(tracks.Value().tracks).Value(), ldm::model::DownloadKind::Hls);

    entry.variant = 3;
    EXPECT_FALSE(ldm::core::TracksForQuality(offer, entry, "https://www.example.test/").Ok());
}

}  // namespace
