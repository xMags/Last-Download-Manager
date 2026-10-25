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
    // Paired by groups: each picture names the delivery of the sound it goes with.
    EXPECT_EQ(video.offer.format, StreamFormat::Hls);
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
        EXPECT_EQ(variant.audioGroup, "file");
        EXPECT_EQ(track.segments.front().url.rfind("https://media.example.test/videoplayback?itag=", 0), 0u);
    }
    EXPECT_EQ(video.offer.variants[2].mimeType, "video/mp4");
    EXPECT_EQ(video.offer.variants[0].mimeType, "video/webm");
}

TEST(SiteVideo, AudioKeepsTheBestStereoTrackPerCodecAndPairsAac)
{
    const SiteVideo video = Parse(ReadFixture("youtube-4k.json"));
    // Not the "-drc" copies, not 5.1 (256, 258), not AC-3/E-AC-3, not the HLS copies 233 and 234.
    ASSERT_EQ(video.offer.audio.size(), 2u);
    std::vector<std::string> ids;
    for (const auto& audio : video.offer.audio) {
        ids.push_back(audio.site->id);
    }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids, (std::vector<std::string>{"140", "251"}));
    for (const auto& audio : video.offer.audio) {
        EXPECT_EQ(audio.isDefault, audio.site->id == "140") << audio.site->id;
        EXPECT_EQ(audio.groupId, "file");
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
              "This live stream offers no way of recording it that LDM has.");
    EXPECT_EQ(Refusal(Answer("[]", R"(,"live_status":"is_upcoming")")), "This video has not started yet.");
}

TEST(SiteVideo, ProtectedOrUnusableFormatsGiveAReason)
{
    EXPECT_EQ(Refusal(Answer("[" + Format("1", "avc1.4d401e", "none", R"(,"height":360,"has_drm":true)") + "]")),
              "This video is protected and cannot be downloaded.");
    EXPECT_EQ(Refusal(Answer("[" + Format("1", "dvh1.05.06", "ac-3", R"(,"height":360)") + "]")),
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
        R"({"format_id":"rtmp","protocol":"rtmp","url":"rtmp://media.example.test/y","vcodec":"avc1.4d401e","acodec":"none","height":1080},)" +
        R"({"format_id":"bad\nid","protocol":"https","url":"https://media.example.test/z","vcodec":"avc1.4d401e","acodec":"none","height":1080},)" +
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
    EXPECT_TRUE(video.offer.variants[0].audioGroup.empty());
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

std::string PlaylistOf(const SiteVideo& video, size_t variant)
{
    const auto* address = std::get_if<ldm::media::PlaylistAddress>(&video.offer.variants.at(variant).source);
    return address ? address->url : std::string("(not a playlist)");
}

TEST(SiteVideo, HlsOnlySitesOfferTheirPlaylistsWithTheSoundInside)
{
    // Dailymotion: four HLS formats, each with picture and sound.
    const SiteVideo video = Parse(ReadFixture("dailymotion.json"));
    EXPECT_EQ(video.extractor, "Dailymotion");
    EXPECT_EQ(VariantIds(video), (std::vector<std::string>{"hls-1080", "hls-720", "hls-480", "hls-380"}));
    EXPECT_EQ(PlaylistOf(video, 0), "https://media.example.test/hls-1080.m3u8");
    EXPECT_TRUE(video.offer.audio.empty());
    const auto list = BuildQualityList(video.offer, video.seconds);
    ASSERT_EQ(list.entries.size(), 4u);
    EXPECT_EQ(list.entries[0].kind, QualityKind::Video);
    EXPECT_FALSE(list.entries[0].audio);
    EXPECT_EQ(QualityLabel(list.entries[0]), "1080p H.264");
}

TEST(SiteVideo, AudioOnlySitesOfferSoundOnly)
{
    // SoundCloud: one HLS AAC track and no picture.
    const SiteVideo video = Parse(ReadFixture("soundcloud.json"));
    EXPECT_TRUE(video.offer.variants.empty());
    ASSERT_EQ(video.offer.audio.size(), 1u);
    const auto& audio = video.offer.audio.front();
    EXPECT_TRUE(audio.groupId.empty());
    ASSERT_TRUE(audio.source);
    EXPECT_TRUE(std::holds_alternative<ldm::media::PlaylistAddress>(*audio.source));
    const auto list = BuildQualityList(video.offer, video.seconds);
    ASSERT_EQ(list.entries.size(), 1u);
    EXPECT_EQ(list.entries[0].kind, QualityKind::AudioOnly);
}

TEST(SiteVideo, SeparateHlsPicturesPairWithHlsSound)
{
    // TED: HLS pictures without sound, HLS sound whose codec is not named, and one https file
    // whose size is not stated, which is not offered beside sized pictures.
    const SiteVideo video = Parse(ReadFixture("ted.json"));
    ASSERT_FALSE(video.offer.variants.empty());
    for (const auto& variant : video.offer.variants) {
        EXPECT_EQ(variant.audioGroup, "hls") << variant.site->id;
        EXPECT_TRUE(variant.resolution) << variant.site->id;
    }
    ASSERT_FALSE(video.offer.audio.empty());
    EXPECT_TRUE(std::any_of(video.offer.audio.begin(), video.offer.audio.end(),
                            [](const auto& a) { return a.isDefault && a.groupId == "hls"; }));
    const auto list = BuildQualityList(video.offer, video.seconds);
    ASSERT_FALSE(list.entries.empty());
    EXPECT_EQ(list.entries[0].kind, QualityKind::Video);
    ASSERT_TRUE(list.entries[0].audio);
    EXPECT_EQ(video.offer.audio[*list.entries[0].audio].groupId, "hls");
}

TEST(SiteVideo, FilesWithoutNamedCodecsAreOfferedWithTheSiteCookies)
{
    // archive.org: three files (ogv, mp4, avi) that name no codecs, with cookies the site set.
    const SiteVideo video = Parse(ReadFixture("archive-org.json"));
    EXPECT_EQ(VariantIds(video), (std::vector<std::string>{"2", "1", "0"}));
    EXPECT_EQ(video.offer.variants[0].site->ext, "avi");
    EXPECT_EQ(video.offer.variants[0].site->bytes, 332243668);
    EXPECT_TRUE(video.offer.variants[0].codecs.empty());
    EXPECT_TRUE(video.offer.variants[0].audioGroup.empty());  // the sound, if any, is inside
    ASSERT_EQ(video.cookies.size(), 3u);  // the same three on every format, kept once
    const auto& csrf = video.cookies.front();
    EXPECT_EQ(csrf.name, "ia-csrf");
    EXPECT_EQ(csrf.domain, "archive.org");
    EXPECT_FALSE(csrf.hostOnly);
    EXPECT_TRUE(csrf.secure);
    EXPECT_EQ(csrf.expires, 1795492351);
    EXPECT_EQ(csrf.path, "/");
}

TEST(SiteVideo, StoryboardsAreLeftOutOfAPastBroadcast)
{
    const SiteVideo video = Parse(ReadFixture("twitch-vod.json"));
    ASSERT_FALSE(video.offer.variants.empty());
    for (size_t i = 0; i < video.offer.variants.size(); ++i) {
        EXPECT_FALSE(video.offer.variants[i].site->id.starts_with("sb"));
        EXPECT_EQ(PlaylistOf(video, i).rfind("https://media.example.test/", 0), 0u);
    }
    EXPECT_EQ(VariantIds(video).front(), "720p-1");  // the higher rate of the two 720p ones
    const auto list = BuildQualityList(video.offer, video.seconds);
    EXPECT_TRUE(std::any_of(list.entries.begin(), list.entries.end(),
                            [](const auto& e) { return e.kind == QualityKind::AudioOnly; }));
}

TEST(SiteVideo, DashPieceListsBecomeSegmentsUnderTheirBase)
{
    const std::string piece = R"("protocol":"http_dash_segments","fragment_base_url":"https://cdn.example.test/v/",)";
    const std::string formats = "[" + std::string(R"({"format_id":"v1",)") + piece +
        R"("fragments":[{"path":"init.mp4"},{"path":"1.m4s","duration":4},{"url":"https://other.example.test/2.m4s","duration":2.5}],)" +
        R"("vcodec":"avc1.64001f","acodec":"none","height":720,"ext":"mp4"},)" +
        R"({"format_id":"a1",)" + piece +
        R"("fragments":[{"path":"a/1.m4s","duration":6.5}],"vcodec":"none","acodec":"mp4a.40.2","ext":"m4a"},)" +
        R"({"format_id":"ranged",)" + piece +
        R"("fragments":[{"path":"x","range":"0-99"}],"vcodec":"avc1.64001f","acodec":"none","height":1080},)" +
        R"({"format_id":"escape",)" + piece +
        R"("fragments":[{"path":"javascript:void"}],"vcodec":"avc1.64001f","acodec":"none","height":1440},)" +
        R"({"format_id":"empty",)" + piece + R"("fragments":[],"vcodec":"avc1.64001f","acodec":"none","height":2160})" + "]";
    const SiteVideo video = Parse(Answer(formats));
    EXPECT_EQ(VariantIds(video), (std::vector<std::string>{"v1"}));
    EXPECT_EQ(video.offer.variants[0].audioGroup, "pieces");
    const auto& track = std::get<TrackSegments>(video.offer.variants[0].source);
    ASSERT_EQ(track.inits.size(), 1u);  // the first piece, without a duration
    EXPECT_EQ(track.inits[0].url, "https://cdn.example.test/v/init.mp4");
    ASSERT_EQ(track.segments.size(), 2u);
    EXPECT_EQ(track.segments[0].url, "https://cdn.example.test/v/1.m4s");
    EXPECT_EQ(track.segments[0].init, 0u);
    EXPECT_EQ(track.segments[1].url, "https://other.example.test/2.m4s");
    // A single piece is the track itself, not an init.
    ASSERT_EQ(video.offer.audio.size(), 1u);
    const auto& sound = std::get<TrackSegments>(*video.offer.audio[0].source);
    EXPECT_TRUE(sound.inits.empty());
    ASSERT_EQ(sound.segments.size(), 1u);
    EXPECT_DOUBLE_EQ(track.targetSeconds, 4);
    ASSERT_EQ(video.offer.audio.size(), 1u);
    EXPECT_EQ(video.offer.audio[0].groupId, "pieces");
    EXPECT_TRUE(video.offer.audio[0].isDefault);
}

TEST(SiteVideo, APictureOnlyPairsWithSoundOfTheSameDelivery)
{
    // A 1080p HLS picture without sound, and sound only as a file: it cannot be offered, as LDM
    // never makes one download of an HLS track and a file.
    const std::string formats = "[" + Format("v360", "avc1.4d401e", "none", R"(,"height":360)") + "," +
        R"({"format_id":"v1080","protocol":"m3u8_native","url":"https://media.example.test/v.m3u8","vcodec":"avc1.640028","acodec":"none","height":1080},)" +
        Format("a", "none", "mp4a.40.2", R"(,"tbr":128)") + "]";
    EXPECT_EQ(VariantIds(Parse(Answer(formats))), (std::vector<std::string>{"v360"}));
}

TEST(SiteVideo, AFileWhosePictureSizeIsUnknownIsOfferedWhenItIsTheOnlyOne)
{
    const SiteVideo video = Parse(Answer(
        "[" + std::string(R"({"format_id":"only","protocol":"https","url":"https://media.example.test/o.mp4","ext":"mp4","tbr":800})") + "]"));
    EXPECT_EQ(VariantIds(video), (std::vector<std::string>{"only"}));
    EXPECT_FALSE(video.offer.variants[0].resolution);
}

TEST(SiteVideo, TheSitesRefererIsKeptAndOtherHeadersAreNot)
{
    const SiteVideo video = Parse(Answer("[" +
        Format("v", "avc1.4d401e", "mp4a.40.2",
               R"(,"height":360,"http_headers":{"Referer":"https://www.example.test/watch/1","User-Agent":"app/1"})") + "," +
        Format("w", "avc1.4d401e", "mp4a.40.2", R"(,"height":480,"http_headers":{"Referer":"https://www.example.test/\r\nX: y"})") +
        "]"));
    ASSERT_EQ(video.offer.variants.size(), 2u);
    EXPECT_TRUE(video.offer.variants[0].site->referer.empty());  // 480p: a line break, refused
    EXPECT_EQ(video.offer.variants[1].site->referer, "https://www.example.test/watch/1");
}

TEST(SiteVideo, SubtitlesAreTheUploadersAndTheAutomaticOnesInTheVideosLanguage)
{
    const std::string subtitles = R"j(,"subtitles":{)j"
        R"j("en":[{"ext":"json3","url":"https://subs.example.test/en.json"},{"ext":"srt","url":"https://subs.example.test/en.srt"},)j"
        R"j({"ext":"vtt","url":"https://subs.example.test/en.vtt","name":"English"}],)j"
        R"j("de-DE":[{"ext":"srt","url":"https://subs.example.test/de.srt","name":"German (Germany)"}],)j"
        R"j("ja":[{"ext":"ttml","url":"https://subs.example.test/ja.ttml"}],)j"
        R"j("el":[{"ext":"vtt","protocol":"m3u8_native","url":"https://subs.example.test/el.m3u8"}],)j"
        R"j("live_chat":[{"ext":"json","url":"https://subs.example.test/chat"}],)j"
        R"j("fr":[{"ext":"vtt","url":"javascript:void"}]},)j"
        R"j("automatic_captions":{"en-orig":[{"ext":"vtt","url":"https://subs.example.test/auto.vtt","name":"English (Original)"}],)j"
        R"j("en":[{"ext":"vtt","url":"https://subs.example.test/auto-en.vtt"}],"af":[{"ext":"vtt","url":"https://subs.example.test/af.vtt"}]})j";
    const SiteVideo video = Parse(Answer("[" + Format("v", "avc1.4d401e", "mp4a.40.2", R"(,"height":360)") + "]", subtitles));
    std::vector<std::string> ids;
    for (const auto& rendition : video.offer.subtitles) {
        ids.push_back(rendition.site ? rendition.site->id : "?");
    }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids, (std::vector<std::string>{"automatic:en-orig", "subtitles:de-DE", "subtitles:el", "subtitles:en"}));
    for (const auto& rendition : video.offer.subtitles) {
        ASSERT_TRUE(rendition.source);
        if (rendition.site->id == "subtitles:en") {
            EXPECT_EQ(rendition.name, "English");
            EXPECT_EQ(rendition.site->ext, "vtt") << "WebVTT over SRT";
            EXPECT_EQ(std::get<TrackSegments>(*rendition.source).segments.front().url, "https://subs.example.test/en.vtt");
        } else if (rendition.site->id == "subtitles:de-DE") {
            EXPECT_EQ(rendition.site->ext, "srt") << "SRT when there is no WebVTT";
        } else if (rendition.site->id == "subtitles:el") {
            EXPECT_TRUE(std::holds_alternative<ldm::media::PlaylistAddress>(*rendition.source));
        } else {
            EXPECT_EQ(rendition.name, "English (Original) (automatic)");
            EXPECT_EQ(rendition.language, "en");
        }
    }
    EXPECT_EQ(BuildQualityList(video.offer, video.seconds).subtitles.size(), 4u);
}

TEST(SiteVideo, ALiveStreamOffersItsHlsPlaylistsForRecording)
{
    const std::string formats = "[" +
        std::string(R"({"format_id":"95","protocol":"m3u8_native","url":"https://live.example.test/720.m3u8","vcodec":"avc1.4d401f","acodec":"mp4a.40.2","height":720},)") +
        R"({"format_id":"300","protocol":"m3u8_native","url":"https://live.example.test/720p60.m3u8","vcodec":"avc1.4d4020","acodec":"mp4a.40.2","height":720,"fps":60},)" +
        R"({"format_id":"137","protocol":"http_dash_segments_generator","url":"https://live.example.test/dash","vcodec":"avc1.640028","acodec":"none","height":1080},)" +
        Format("18", "avc1.42001e", "mp4a.40.2", R"(,"height":360)") + "]";
    const SiteVideo video = Parse(Answer(formats, R"(,"live_status":"is_live","is_live":true)"));
    EXPECT_TRUE(video.offer.live);
    EXPECT_FALSE(video.offer.seconds) << "a recording's length depends on how long it runs";
    EXPECT_EQ(VariantIds(video), (std::vector<std::string>{"300", "95"})) << "no files of a stream that has no end";
    const auto list = BuildQualityList(video.offer, video.offer.seconds);
    ASSERT_EQ(list.entries.size(), 2u);
    EXPECT_FALSE(list.entries[0].approximateBytes);
}

TEST(SiteVideo, APlaylistListsItsVideosPagesAndAChannelItsTabsVideos)
{
    const std::string playlist = R"j({"_type":"playlist","id":"PL1","title":"Stories","entries":[)j"
        R"j({"_type":"url","ie_key":"Youtube","id":"GvgqDSnpRQM","url":"https://www.youtube.com/watch?v=GvgqDSnpRQM","title":"Andrew","duration":90},)j"
        R"j({"_type":"url","id":"x","url":"https://www.youtube.com/watch?v=fIc_VEQ7Vo0","title":null,"duration":null},)j"
        R"j({"_type":"url","id":"bad","url":"javascript:void"},)j"
        R"j({"_type":"playlist","title":"Blender - Shorts","entries":[{"_type":"url","url":"https://www.youtube.com/shorts/YsKxkP_kGbE","title":"Cube"}]})j"
        R"j(]})j";
    const auto parsed = ldm::media::ParseSiteAnswer(playlist);
    ASSERT_TRUE(parsed.Ok()) << parsed.GetError().message;
    const auto* list = std::get_if<ldm::media::SitePlaylist>(&parsed.Value());
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->title, "Stories");
    ASSERT_EQ(list->entries.size(), 3u);
    EXPECT_EQ(list->entries[0].url, "https://www.youtube.com/watch?v=GvgqDSnpRQM");
    EXPECT_EQ(list->entries[0].title, "Andrew");
    EXPECT_EQ(list->entries[0].seconds, 90);
    EXPECT_TRUE(list->entries[1].title.empty());
    EXPECT_EQ(list->entries[2].group, "Blender - Shorts");
    EXPECT_FALSE(list->entries[0].video);

    // A single video is still one.
    const auto one = ldm::media::ParseSiteAnswer(ReadFixture("dailymotion.json"));
    ASSERT_TRUE(one.Ok());
    EXPECT_TRUE(std::holds_alternative<SiteVideo>(one.Value()));
    EXPECT_EQ(Refusal(playlist), "This page is a playlist. Open one of its videos to download it.");
}

TEST(SiteVideo, AnOrdinaryPagesVideosAreGivenInFull)
{
    const std::string page = R"j({"_type":"playlist","id":"html5_video","title":"HTML Video","entries":[)j"
        R"j({"id":"v-1","title":"HTML Video (1)","webpage_url":"https://www.example.test/video.html","formats":[)j"
        R"j({"format_id":"mp4","protocol":"https","url":"https://www.example.test/bbb.mp4","ext":"mp4"}]},)j"
        R"j({"id":"v-2","title":"Nothing","formats":[{"format_id":"x","protocol":"rtmp","url":"rtmp://x"}]}]})j";
    const auto parsed = ldm::media::ParseSiteAnswer(page);
    ASSERT_TRUE(parsed.Ok()) << parsed.GetError().message;
    const auto& list = std::get<ldm::media::SitePlaylist>(parsed.Value());
    ASSERT_EQ(list.entries.size(), 1u) << "a video with nothing LDM can download is left out";
    ASSERT_TRUE(list.entries[0].video);
    EXPECT_EQ(list.entries[0].url, "https://www.example.test/video.html");
    EXPECT_EQ(VariantIds(*list.entries[0].video), (std::vector<std::string>{"mp4"}));

    const auto empty = ldm::media::ParseSiteAnswer(R"({"_type":"playlist","id":"e","entries":[]})");
    ASSERT_FALSE(empty.Ok());
    EXPECT_EQ(empty.GetError().message, "This page lists no videos LDM can download.");
}

TEST(SiteVideo, AListLongerThanTheLimitIsCut)
{
    std::string page = R"({"_type":"playlist","id":"long","entries":[)";
    for (size_t i = 0; i < ldm::media::kMaxSitePlaylistEntries + 3; ++i) {
        page += (i == 0 ? "" : ",") + std::string(R"({"_type":"url","url":"https://v.example.test/)") + std::to_string(i) + R"("})";
    }
    page += "]}";
    const auto parsed = ldm::media::ParseSiteAnswer(page);
    ASSERT_TRUE(parsed.Ok());
    const auto& list = std::get<ldm::media::SitePlaylist>(parsed.Value());
    EXPECT_EQ(list.entries.size(), ldm::media::kMaxSitePlaylistEntries);
    EXPECT_TRUE(list.truncated);
}

TEST(SiteVideo, CookieTextThatDoesNotReadIsNotTaken)
{
    const auto cookiesOf = [](const std::string& text) {
        return Parse(Answer("[" + Format("v", "avc1.4d401e", "mp4a.40.2", R"(,"height":360,"cookies":")" + text + "\"") + "]"))
            .cookies.size();
    };
    EXPECT_EQ(cookiesOf("a=1; Domain=.example.test; Path=/"), 1u);
    EXPECT_EQ(cookiesOf("a=1; Path=/"), 0u);                          // no Domain
    EXPECT_EQ(cookiesOf("Domain=.example.test; a=1"), 0u);            // an attribute first
    EXPECT_EQ(cookiesOf("a=1; Domain=.example.test; junk"), 0u);      // neither cookie nor attribute
    EXPECT_EQ(cookiesOf("a=1; Domain=exa mple.test"), 0u);            // not a host name
    EXPECT_EQ(cookiesOf("a=1; Domain=example.test; Path=nope"), 0u);  // a path starts with '/'
}

}  // namespace
