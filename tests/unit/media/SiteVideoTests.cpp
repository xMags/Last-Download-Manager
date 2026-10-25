#include "media/QualityList.h"
#include "media/SiteVideo.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <variant>
#include <vector>

namespace {

using ldm::media::BuildQualityList;
using ldm::media::ParseSiteVideo;
using ldm::media::QualityKind;
using ldm::media::QualityLabel;
using ldm::media::SiteVideo;
using ldm::media::StreamFormat;
using ldm::media::TrackSegments;

// Answers of yt-dlp 2026.10.21 for two public YouTube videos, kept to the fields LDM reads and
// with every address replaced (tests/fixtures/media/site).
std::string ReadFixture(const char* name)
{
    std::ifstream in(std::filesystem::path(LDM_MEDIA_FIXTURES) / "site" / name, std::ios::binary);
    EXPECT_TRUE(in) << name;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

SiteVideo Parse(const std::string& json)
{
    auto parsed = ParseSiteVideo(json);
    EXPECT_TRUE(parsed.Ok()) << (parsed.Ok() ? "" : parsed.GetError().message);
    return parsed.Ok() ? std::move(parsed).Value() : SiteVideo{};
}

std::string Refusal(const std::string& json)
{
    const auto parsed = ParseSiteVideo(json);
    return parsed.Ok() ? std::string("(accepted)") : parsed.GetError().message;
}

std::vector<std::string> VariantIds(const SiteVideo& video)
{
    std::vector<std::string> ids;
    for (const auto& variant : video.offer.variants) {
        ids.push_back(variant.site ? variant.site->id : "?");
    }
    return ids;
}

// A minimal answer around `formats`, a JSON array.
std::string Answer(const std::string& formats, const std::string& extra = "")
{
    return R"({"id":"abc","title":"T","duration":10)" + extra + R"(,"formats":)" + formats + "}";
}

std::string Format(const std::string& id, const std::string& vcodec, const std::string& acodec,
                   const std::string& more = "")
{
    return R"({"format_id":")" + id + R"(","protocol":"https","url":"https://media.example.test/)" + id +
           R"(","ext":"mp4","vcodec":")" + vcodec + R"(","acodec":")" + acodec + R"(")" + more + "}";
}

TEST(SiteVideo, LongVideoGivesOnePicturePerHeightAndFrameRatePreferringH264)
{
    const SiteVideo video = Parse(ReadFixture("youtube-4k.json"));
    EXPECT_EQ(video.id, "aqz-KE-bpKQ");
    EXPECT_EQ(video.title, "Big Buck Bunny 60fps 4K - Official Blender Foundation Short Film");
    ASSERT_TRUE(video.seconds);
    EXPECT_DOUBLE_EQ(*video.seconds, 635);
    EXPECT_EQ(video.offer.format, StreamFormat::Dash);
    // VP9 above 1080p, where the site has no H.264; never the AV1 copies, the storyboards, the HLS
    // duplicates or the low combined format 18.
    EXPECT_EQ(VariantIds(video), (std::vector<std::string>{"315", "308", "299", "298", "136", "135", "134", "133", "160"}));
    for (const auto& variant : video.offer.variants) {
        ASSERT_TRUE(variant.site);
        EXPECT_TRUE(variant.site->bytes) << variant.site->id;
        const auto& track = std::get<TrackSegments>(variant.source);
        ASSERT_EQ(track.segments.size(), 1u);  // one whole file
        EXPECT_FALSE(track.segments.front().range);
        EXPECT_TRUE(track.inits.empty());
        EXPECT_EQ(track.segments.front().url.rfind("https://media.example.test/videoplayback?itag=", 0), 0u);
    }
    EXPECT_EQ(video.offer.variants[2].mimeType, "video/mp4");
    EXPECT_EQ(video.offer.variants[0].mimeType, "video/webm");
}

TEST(SiteVideo, AudioKeepsTheBestStereoTrackPerCodecAndPairsAac)
{
    const SiteVideo video = Parse(ReadFixture("youtube-4k.json"));
    // Not the "-drc" copies, not 5.1 (256, 258), not AC-3/E-AC-3 or the unknown codec of 773.
    ASSERT_EQ(video.offer.audio.size(), 2u);
    std::vector<std::string> ids;
    for (const auto& audio : video.offer.audio) {
        ids.push_back(audio.site->id);
    }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids, (std::vector<std::string>{"140", "251"}));
    for (const auto& audio : video.offer.audio) {
        EXPECT_EQ(audio.isDefault, audio.site->id == "140") << audio.site->id;
    }
}

TEST(SiteVideo, TheQualityListShowsRealSizesForEveryPicture)
{
    const SiteVideo video = Parse(ReadFixture("youtube-4k.json"));
    const auto list = BuildQualityList(video.offer, video.seconds);
    ASSERT_FALSE(list.entries.empty());
    const auto& best1080 = list.entries[2];
    ASSERT_EQ(best1080.kind, QualityKind::Video);
    EXPECT_EQ(QualityLabel(best1080), "1080p60 H.264");
    ASSERT_TRUE(best1080.audio);
    EXPECT_EQ(video.offer.audio[*best1080.audio].site->id, "140");
    // 299 and 140 together: 257,619,653 + 10,271,496 bytes, within the rounding of bits per second.
    ASSERT_TRUE(best1080.approximateBytes);
    EXPECT_NEAR(static_cast<double>(*best1080.approximateBytes), 267'891'149.0, 200.0);
    size_t audioOnly = 0;
    for (const auto& entry : list.entries) {
        audioOnly += entry.kind == QualityKind::AudioOnly ? 1 : 0;
    }
    EXPECT_EQ(audioOnly, 2u);
}

TEST(SiteVideo, ShortVideoPicksTheHigherRateOfTwoEqualPictures)
{
    const SiteVideo video = Parse(ReadFixture("youtube-short.json"));
    EXPECT_EQ(video.id, "jNQXAC9IVRw");
    EXPECT_EQ(VariantIds(video), (std::vector<std::string>{"133", "160"}));
    ASSERT_TRUE(video.offer.variants[0].resolution);
    EXPECT_EQ(video.offer.variants[0].resolution->width, 320);
    EXPECT_EQ(video.offer.variants[0].resolution->height, 240);
    EXPECT_EQ(video.offer.variants[0].site->bytes, 433081);
}

TEST(SiteVideo, PlaylistsLiveAndUpcomingVideosAreRefusedWithWordsToShow)
{
    EXPECT_EQ(Refusal(R"({"_type":"playlist","id":"PL1","entries":[]})"),
              "This page is a playlist. Open one of its videos to download it.");
    EXPECT_EQ(Refusal(R"({"_type":"url","id":"x"})"), "This page is not a single video.");
    EXPECT_EQ(Refusal(Answer("[" + Format("1", "avc1.4d401e", "none", R"(,"height":360)") + "]", R"(,"is_live":true)")),
              "Live streams from this site cannot be downloaded yet.");
    EXPECT_EQ(Refusal(Answer("[]", R"(,"live_status":"is_upcoming")")), "This video has not started yet.");
}

TEST(SiteVideo, ProtectedOrUnusableFormatsGiveAReason)
{
    EXPECT_EQ(Refusal(Answer("[" + Format("1", "avc1.4d401e", "none", R"(,"height":360,"has_drm":true)") + "]")),
              "This video is protected and cannot be downloaded.");
    EXPECT_EQ(Refusal(Answer("[" + Format("1", "hvc1.1", "none", R"(,"height":360)") + "]")),
              "This video has no format LDM can download.");
    EXPECT_EQ(Refusal(Answer("[]")), "This video has no format LDM can download.");
}

TEST(SiteVideo, MalformedAnswersAreRefused)
{
    const std::string unreadable = "The video site's answer could not be read.";
    EXPECT_EQ(Refusal(""), unreadable);
    EXPECT_EQ(Refusal("WARNING: something\n{}"), unreadable);
    EXPECT_EQ(Refusal("[]"), unreadable);
    EXPECT_EQ(Refusal(R"({"title":"no id","formats":[]})"), unreadable);
    EXPECT_EQ(Refusal(R"({"id":"x","formats":{}})"), unreadable);
    EXPECT_EQ(Refusal(R"({"id":"x","formats":[)"), unreadable);
}

TEST(SiteVideo, FormatsWithUnsafeOrMissingFieldsAreLeftOut)
{
    const std::string formats = "[" +
        Format("ok", "avc1.4d401e", "none", R"(,"height":360)") + "," +
        R"({"format_id":"file","protocol":"https","url":"file:///C:/Windows/win.ini","vcodec":"avc1.4d401e","acodec":"none","height":720},)" +
        R"({"format_id":"js","protocol":"https","url":"javascript:void","vcodec":"avc1.4d401e","acodec":"none","height":720},)" +
        R"({"format_id":"hls","protocol":"m3u8_native","url":"https://media.example.test/a.m3u8","vcodec":"avc1.4d401e","acodec":"none","height":1080},)" +
        R"({"protocol":"https","url":"https://media.example.test/noid","vcodec":"avc1.4d401e","acodec":"none","height":1080},)" +
        R"({"format_id":"noheight","protocol":"https","url":"https://media.example.test/x","vcodec":"avc1.4d401e","acodec":"none"},)" +
        R"({"format_id":"unknown","protocol":"https","url":"https://media.example.test/y","height":1080},)" +
        R"("not an object",)" +
        Format("a", "none", "mp4a.40.2", R"(,"tbr":128,"audio_channels":2)") + "]";
    const SiteVideo video = Parse(Answer(formats));
    EXPECT_EQ(VariantIds(video), (std::vector<std::string>{"ok"}));
    ASSERT_EQ(video.offer.audio.size(), 1u);
}

TEST(SiteVideo, AnAddressLongerThanTheLimitIsLeftOut)
{
    const std::string longUrl = "https://media.example.test/" + std::string(ldm::media::kMaxSiteUrlBytes, 'a');
    const std::string formats = "[" + Format("ok", "avc1.4d401e", "none", R"(,"height":360)") + "," +
                                R"({"format_id":"long","protocol":"https","url":")" + longUrl +
                                R"(","vcodec":"avc1.4d401e","acodec":"none","height":720},)" +
                                Format("a", "none", "opus", R"(,"tbr":128)") + "]";
    EXPECT_EQ(VariantIds(Parse(Answer(formats))), (std::vector<std::string>{"ok"}));
}

TEST(SiteVideo, OnlyTheFirstFormatsUpToTheLimitAreRead)
{
    std::string formats = "[" + Format("audio", "none", "opus", R"(,"tbr":128)");
    for (size_t i = 1; i < ldm::media::kMaxSiteFormats; ++i) {
        formats += "," + Format("v" + std::to_string(i), "avc1.4d401e", "none", R"(,"height":360,"tbr":1)");
    }
    formats += "," + Format("late", "avc1.4d401e", "none", R"(,"height":720)") + "]";
    // The 720p format is number 501: never read. Of the equal 360p ones, the first is kept.
    EXPECT_EQ(VariantIds(Parse(Answer(formats))), (std::vector<std::string>{"v1"}));
}

TEST(SiteVideo, SitesWithOnlyCombinedFormatsOfferThemWithTheirOwnSound)
{
    const std::string formats = "[" +
        Format("low", "avc1.42001e", "mp4a.40.2", R"(,"height":360,"tbr":500)") + "," +
        Format("high", "avc1.64001f", "mp4a.40.2", R"(,"height":720,"tbr":1500,"filesize":1875000)") + "]";
    const SiteVideo video = Parse(Answer(formats));
    EXPECT_EQ(video.offer.format, StreamFormat::Hls);  // the sound is in each picture's file
    EXPECT_EQ(VariantIds(video), (std::vector<std::string>{"high", "low"}));
    const auto list = BuildQualityList(video.offer, video.seconds);
    ASSERT_EQ(list.entries.size(), 2u);
    EXPECT_EQ(list.entries[0].kind, QualityKind::Video);
    EXPECT_FALSE(list.entries[0].audio);
    ASSERT_TRUE(list.entries[0].approximateBytes);
    EXPECT_EQ(*list.entries[0].approximateBytes, 1'875'000);
}

TEST(SiteVideo, TheOriginalLanguageIsPairedOverADub)
{
    const std::string formats = "[" + Format("v", "avc1.4d401e", "none", R"(,"height":360)") + "," +
        Format("dub-aac", "none", "mp4a.40.2", R"(,"tbr":128,"language":"de","language_preference":-1)") + "," +
        Format("orig-opus", "none", "opus", R"(,"tbr":130,"language":"en","language_preference":10)") + "," +
        Format("orig-aac", "none", "mp4a.40.2", R"(,"tbr":128,"language":"en","language_preference":10)") + "]";
    const SiteVideo video = Parse(Answer(formats));
    ASSERT_EQ(video.offer.audio.size(), 3u);
    for (const auto& audio : video.offer.audio) {
        EXPECT_EQ(audio.isDefault, audio.site->id == "orig-aac") << audio.site->id;
    }
}

}  // namespace
