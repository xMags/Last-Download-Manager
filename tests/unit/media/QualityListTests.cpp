#include "media/DashManifest.h"
#include "media/HlsPlaylist.h"
#include "media/QualityList.h"
#include "platform/win/XmlLiteReader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <variant>

namespace {

using ldm::media::BuildQualityList;
using ldm::media::MediaOffer;
using ldm::media::QualityKind;
using ldm::media::QualityLabel;

std::string ReadFixture(const char* path)
{
    std::ifstream in(std::filesystem::path(LDM_MEDIA_FIXTURES) / path, std::ios::binary);
    EXPECT_TRUE(in) << path;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

TEST(QualityList, AVariantThatSaysNothingAboutItsStreamsIsTakenAsVideo)
{
    MediaOffer offer;
    offer.variants.emplace_back();  // a media playlist on its own: no codecs, no resolution
    ldm::media::Variant sound;
    sound.codecs = "mp4a.40.2";
    offer.variants.push_back(sound);
    const auto list = BuildQualityList(offer, std::nullopt);
    ASSERT_EQ(list.entries.size(), 2u);
    EXPECT_EQ(list.entries[0].kind, QualityKind::Video);
    EXPECT_EQ(QualityLabel(list.entries[0]), "Media");

    // A site's file whose type is a video, though it states nothing else (a Kick or Facebook clip).
    MediaOffer site;
    site.variants.emplace_back();
    site.variants[0].mimeType = "video/mp4";
    EXPECT_EQ(QualityLabel(BuildQualityList(site, std::nullopt).entries[0]), "Video");
    EXPECT_EQ(list.entries[1].kind, QualityKind::AudioOnly) << "codecs that name sound only still mean sound";
}

MediaOffer HlsMaster()
{
    auto parsed = ldm::media::ParseHls(ReadFixture("hls/master.m3u8"), "https://media.example.test/show/ep1/master.m3u8");
    EXPECT_TRUE(parsed);
    return parsed ? std::get<MediaOffer>(std::move(parsed).Value()) : MediaOffer{};
}

MediaOffer DashTemplate()
{
    auto root = ldm::platform::ReadXml(ReadFixture("dash/template-number.mpd"));
    EXPECT_TRUE(root);
    auto offer = ldm::media::ParseMpd(root.Value(), "https://media.example.test/vod/movie/manifest.mpd",
                                      std::chrono::system_clock::now());
    EXPECT_TRUE(offer);
    return offer ? std::move(offer).Value() : MediaOffer{};
}

TEST(QualityList, HlsVariantsComeFirstWithTheirAudioGroupsTrack)
{
    const MediaOffer offer = HlsMaster();
    const auto list = BuildQualityList(offer, 600.0);
    ASSERT_FALSE(list.drm);
    ASSERT_GE(list.entries.size(), 4u);

    const auto& best = list.entries[0];
    EXPECT_EQ(best.kind, QualityKind::Video);
    EXPECT_EQ(best.height, 1080);
    EXPECT_EQ(best.variant, 0u);
    EXPECT_EQ(best.audio, 0u) << "the default track of group aac";
    EXPECT_EQ(best.videoCodec, "H.264");
    EXPECT_EQ(best.audioCodec, "AAC");
    EXPECT_EQ(best.bitsPerSecond, 4'500'000u) << "the average bandwidth when given; HLS rates include the sound";
    EXPECT_EQ(best.approximateBytes, 4'500'000LL * 600 / 8);
    EXPECT_EQ(QualityLabel(best), "1080p H.264");

    const auto& sd = list.entries[1];
    EXPECT_EQ(sd.kind, QualityKind::Video);
    EXPECT_EQ(sd.height, 480);
    EXPECT_FALSE(sd.audio) << "group muxed has no separate track: its sound is in the variant";

    // The 1080p variant's sound is separate, so it can also be had without it.
    const auto videoOnly = std::find_if(list.entries.begin(), list.entries.end(),
                                        [](const auto& e) { return e.kind == QualityKind::VideoOnly; });
    ASSERT_NE(videoOnly, list.entries.end());
    EXPECT_EQ(videoOnly->variant, 0u);
    EXPECT_EQ(QualityLabel(*videoOnly), "Video only 1080p H.264");

    size_t audioOnly = 0;
    for (const auto& entry : list.entries) {
        audioOnly += entry.kind == QualityKind::AudioOnly ? 1 : 0;
    }
    EXPECT_EQ(audioOnly, 3u) << "the audio-only variant and the two tracks with playlists";
    EXPECT_EQ(list.subtitles.size(), 2u);
}

TEST(QualityList, DashVideoIsPairedWithTheMainAudio)
{
    const MediaOffer offer = DashTemplate();
    const auto list = BuildQualityList(offer, offer.seconds);
    ASSERT_EQ(list.entries.size(), 5u) << "2 videos with sound, 2 without, 1 audio";
    EXPECT_EQ(list.entries[0].kind, QualityKind::Video);
    EXPECT_EQ(list.entries[0].height, 720);
    EXPECT_EQ(list.entries[0].audio, 0u);
    EXPECT_EQ(list.entries[0].bitsPerSecond, 3'000'000u + 128'000u) << "DASH rates are per track: added";
    EXPECT_EQ(list.entries[0].approximateBytes, static_cast<std::int64_t>((3'000'000.0 + 128'000.0) * 9.5 / 8));
    EXPECT_EQ(list.entries[1].height, 360);
    EXPECT_EQ(list.entries[2].kind, QualityKind::VideoOnly);
    EXPECT_EQ(list.entries[4].kind, QualityKind::AudioOnly);
    EXPECT_EQ(QualityLabel(list.entries[4]), "Audio en AAC 128 kbps");
    EXPECT_EQ(list.subtitles.size(), 1u);
}

TEST(QualityList, ProtectedStreamsOfferNothing)
{
    MediaOffer offer = DashTemplate();
    offer.drm = ldm::media::DrmInfo{{"Widevine"}};
    const auto list = BuildQualityList(offer, offer.seconds);
    ASSERT_TRUE(list.drm);
    EXPECT_TRUE(list.entries.empty());
}

TEST(QualityList, SizesNeedALength)
{
    const auto list = BuildQualityList(HlsMaster(), std::nullopt);
    ASSERT_FALSE(list.entries.empty());
    EXPECT_FALSE(list.entries[0].approximateBytes);
}

TEST(QualityList, CodecFamilies)
{
    EXPECT_EQ(ldm::media::VideoCodecFamily("mp4a.40.2, avc1.64001f"), "H.264");
    EXPECT_EQ(ldm::media::VideoCodecFamily("hvc1.2.4.L123.B0"), "H.265");
    EXPECT_EQ(ldm::media::VideoCodecFamily("vp09.00.10.08"), "VP9");
    EXPECT_EQ(ldm::media::VideoCodecFamily("AV01.0.05M.08"), "AV1");
    EXPECT_EQ(ldm::media::VideoCodecFamily("mp4a.40.2"), "");
    EXPECT_EQ(ldm::media::AudioCodecFamily("avc1.4d401e,mp4a.40.2"), "AAC");
    EXPECT_EQ(ldm::media::AudioCodecFamily("ec-3"), "E-AC-3");
    EXPECT_EQ(ldm::media::AudioCodecFamily("opus"), "Opus");
}

TEST(QualityList, LabelsShowHighFrameRates)
{
    ldm::media::QualityEntry entry;
    entry.height = 1080;
    entry.frameRate = 59.94;
    entry.videoCodec = "VP9";
    EXPECT_EQ(QualityLabel(entry), "1080p60 VP9");
    entry.frameRate = 29.97;
    EXPECT_EQ(QualityLabel(entry), "1080p VP9");
}

}  // namespace
