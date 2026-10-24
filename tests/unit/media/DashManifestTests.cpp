// The DASH parser, fed through the real XmlLite reader as LDM will feed it.
#include "media/DashManifest.h"
#include "platform/win/XmlLiteReader.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <variant>
#include <vector>

namespace {

using ldm::media::MediaOffer;
using ldm::media::TrackSegments;

constexpr char kManifestUrl[] = "https://media.example.test/vod/movie/manifest.mpd";

// 2026-10-24T10:00:00Z, the live fixture's availabilityStartTime.
std::chrono::system_clock::time_point LiveStart()
{
    return std::chrono::sys_days(std::chrono::year{2026} / 10 / 24) + std::chrono::hours(10);
}

std::string ReadFixture(const char* name)
{
    std::ifstream in(std::filesystem::path(LDM_MEDIA_FIXTURES) / "dash" / name, std::ios::binary);
    EXPECT_TRUE(in) << name;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

ldm::base::Result<MediaOffer> Parse(const std::string& xml, std::chrono::system_clock::time_point now = LiveStart())
{
    auto root = ldm::platform::ReadXml(xml);
    if (!root) {
        return root.GetError();
    }
    return ldm::media::ParseMpd(root.Value(), kManifestUrl, now);
}

MediaOffer ParseOk(const std::string& xml, std::chrono::system_clock::time_point now = LiveStart())
{
    auto offer = Parse(xml, now);
    EXPECT_TRUE(offer) << (offer ? "" : offer.GetError().message);
    return offer ? std::move(offer).Value() : MediaOffer{};
}

std::string Refusal(const std::string& xml, std::chrono::system_clock::time_point now = LiveStart())
{
    const auto offer = Parse(xml, now);
    if (offer) {
        ADD_FAILURE() << "accepted:\n" << xml;
        return {};
    }
    return offer.GetError().message;
}

const TrackSegments& Segments(const ldm::media::SegmentSource& source)
{
    static const TrackSegments kNone;
    const auto* segments = std::get_if<TrackSegments>(&source);
    EXPECT_TRUE(segments) << "a DASH track carries its segments";
    return segments ? *segments : kNone;
}

// A one-representation MPD around `segmentInfo`, for the edge cases.
std::string SingleTrack(const std::string& mpdAttributes, const std::string& segmentInfo)
{
    return "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" " + mpdAttributes +
           "><Period><AdaptationSet contentType=\"video\" mimeType=\"video/mp4\">" + segmentInfo +
           "<Representation id=\"v\" bandwidth=\"1000\"/></AdaptationSet></Period></MPD>";
}

TEST(DashManifest, ReadsTemplatesWithNumbersAndInheritance)
{
    const MediaOffer offer = ParseOk(ReadFixture("template-number.mpd"));
    EXPECT_EQ(offer.format, ldm::media::StreamFormat::Dash);
    EXPECT_FALSE(offer.live);
    EXPECT_FALSE(offer.drm);
    EXPECT_DOUBLE_EQ(offer.seconds.value_or(0), 9.5);

    ASSERT_EQ(offer.variants.size(), 2u) << "the trick-mode set is left out";
    const auto& hd = offer.variants[0];
    EXPECT_EQ(hd.bandwidth, 3'000'000u);
    ASSERT_TRUE(hd.resolution);
    EXPECT_EQ(hd.resolution->width, 1280);
    EXPECT_NEAR(hd.frameRate.value_or(0), 29.97, 0.001);
    EXPECT_EQ(hd.codecs, "avc1.64001f");
    EXPECT_EQ(hd.mimeType, "video/mp4");
    EXPECT_TRUE(hd.audioGroup.empty());
    const TrackSegments& hdSegments = Segments(hd.source);
    ASSERT_EQ(hdSegments.inits.size(), 1u);
    EXPECT_EQ(hdSegments.inits[0].url, "https://media.example.test/vod/movie/media/720p/init.mp4");
    ASSERT_EQ(hdSegments.segments.size(), 5u) << "9.5 s in 2 s segments";
    EXPECT_EQ(hdSegments.segments[0].url, "https://media.example.test/vod/movie/media/720p/seg-00001.m4s");
    EXPECT_EQ(hdSegments.segments[4].url, "https://media.example.test/vod/movie/media/720p/seg-00005.m4s");
    EXPECT_EQ(hdSegments.segments[4].sequence, 5u);
    EXPECT_DOUBLE_EQ(hdSegments.segments[0].seconds, 2);
    EXPECT_DOUBLE_EQ(hdSegments.segments[4].seconds, 1.5) << "the last segment ends with the period";
    EXPECT_EQ(hdSegments.segments[0].init, 0u);
    EXPECT_DOUBLE_EQ(ldm::media::TotalSeconds(hdSegments), 9.5);
    EXPECT_DOUBLE_EQ(hdSegments.targetSeconds, 2);

    const TrackSegments& sdSegments = Segments(offer.variants[1].source);
    EXPECT_EQ(sdSegments.segments[0].url, "https://cdn2.example.test/low/360p/seg-00001.m4s");
    EXPECT_DOUBLE_EQ(offer.variants[1].frameRate.value_or(0), 25);

    ASSERT_EQ(offer.audio.size(), 1u);
    const auto& audio = offer.audio[0];
    EXPECT_EQ(audio.groupId, "2");
    EXPECT_EQ(audio.name, "English stereo");
    EXPECT_EQ(audio.language, "en");
    EXPECT_TRUE(audio.isDefault);
    EXPECT_EQ(audio.channels, 2);
    EXPECT_EQ(audio.bandwidth, 128'000u);
    EXPECT_EQ(audio.codecs, "mp4a.40.2");
    ASSERT_TRUE(audio.source);
    const TrackSegments& audioSegments = Segments(*audio.source);
    ASSERT_EQ(audioSegments.segments.size(), 5u);
    EXPECT_EQ(audioSegments.segments[2].url, "https://media.example.test/vod/movie/media/audio/aac/3.m4s");

    ASSERT_EQ(offer.subtitles.size(), 1u) << "the thumbnails are left out";
    const auto& subtitles = offer.subtitles[0];
    EXPECT_TRUE(subtitles.forced);
    EXPECT_EQ(subtitles.name, "de") << "no Label: the language";
    ASSERT_TRUE(subtitles.source);
    const TrackSegments& subtitleSegments = Segments(*subtitles.source);
    EXPECT_TRUE(subtitleSegments.inits.empty());
    ASSERT_EQ(subtitleSegments.segments.size(), 1u) << "one file, downloaded whole";
    EXPECT_EQ(subtitleSegments.segments[0].url, "https://media.example.test/vod/movie/media/subs/de.vtt");
    EXPECT_DOUBLE_EQ(subtitleSegments.segments[0].seconds, 9.5);
}

TEST(DashManifest, ReadsATimelineWithRepeatsAndTimes)
{
    const MediaOffer offer = ParseOk(ReadFixture("timeline.mpd"));
    ASSERT_EQ(offer.variants.size(), 1u);
    const auto& variant = offer.variants[0];
    ASSERT_TRUE(variant.resolution) << "width and height inherited from the adaptation set";
    EXPECT_EQ(variant.resolution->height, 1080);
    const TrackSegments& track = Segments(variant.source);
    ASSERT_EQ(track.inits.size(), 1u);
    EXPECT_EQ(track.inits[0].url, "https://media.example.test/vod/movie/init-5000000.mp4");
    const std::vector<std::uint64_t> times{900000, 1080000, 1260000, 1440000, 1530000, 1620000, 1710000};
    ASSERT_EQ(track.segments.size(), times.size()) << "r=\"-1\" repeats to the period's end";
    for (size_t i = 0; i < times.size(); ++i) {
        EXPECT_EQ(track.segments[i].url,
                  "https://media.example.test/vod/movie/v-5000000-" + std::to_string(times[i]) + ".m4s");
        EXPECT_EQ(track.segments[i].sequence, i + 1);
    }
    EXPECT_DOUBLE_EQ(track.segments[0].seconds, 2);
    EXPECT_DOUBLE_EQ(track.segments[3].seconds, 1);
    EXPECT_DOUBLE_EQ(ldm::media::TotalSeconds(track), 10);
}

TEST(DashManifest, ReadsSegmentListsAndDownloadsSegmentBaseFilesWhole)
{
    const MediaOffer offer = ParseOk(ReadFixture("list-and-base.mpd"));
    ASSERT_EQ(offer.variants.size(), 2u);

    const TrackSegments& list = Segments(offer.variants[0].source);
    ASSERT_EQ(list.inits.size(), 1u);
    EXPECT_EQ(list.inits[0].url, "https://media.example.test/vod/movie/video-init.mp4");
    ASSERT_TRUE(list.inits[0].range);
    EXPECT_EQ(list.inits[0].range->offset, 0u);
    EXPECT_EQ(list.inits[0].range->length, 800u);
    ASSERT_EQ(list.segments.size(), 2u);
    EXPECT_EQ(list.segments[0].url, "https://media.example.test/vod/movie/video.mp4") << "no @media: the BaseURL";
    ASSERT_TRUE(list.segments[0].range);
    EXPECT_EQ(list.segments[0].range->offset, 800u);
    EXPECT_EQ(list.segments[0].range->length, 1000u);
    EXPECT_EQ(list.segments[1].url, "https://media.example.test/vod/movie/other.mp4");
    EXPECT_EQ(list.segments[1].range->length, 500u);
    EXPECT_DOUBLE_EQ(list.segments[1].seconds, 3);

    const TrackSegments& whole = Segments(offer.variants[1].source);
    EXPECT_TRUE(whole.inits.empty()) << "the init section is inside the file";
    ASSERT_EQ(whole.segments.size(), 1u);
    EXPECT_EQ(whole.segments[0].url, "https://media.example.test/vod/movie/whole.mp4");
    EXPECT_FALSE(whole.segments[0].range);
    EXPECT_DOUBLE_EQ(whole.segments[0].seconds, 6);
}

TEST(DashManifest, ReadsTheLiveWindowAtAGivenTime)
{
    const std::string xml = ReadFixture("live.mpd");
    // 60.5 s in: 15 segments are complete; the 30 s window starts inside the 8th.
    const MediaOffer offer = ParseOk(xml, LiveStart() + std::chrono::milliseconds(60'500));
    EXPECT_TRUE(offer.live);
    EXPECT_FALSE(offer.seconds);
    ASSERT_EQ(offer.variants.size(), 1u);
    const TrackSegments& track = Segments(offer.variants[0].source);
    EXPECT_TRUE(track.live);
    ASSERT_EQ(track.segments.size(), 8u);
    EXPECT_EQ(track.segments.front().sequence, 17u) << "startNumber 10 plus 7 segments before the window";
    EXPECT_EQ(track.segments.back().sequence, 24u);
    EXPECT_EQ(track.segments.back().url, "https://media.example.test/vod/movie/live-24.m4s");

    const MediaOffer early = ParseOk(xml, LiveStart() - std::chrono::minutes(1));
    EXPECT_TRUE(Segments(early.variants[0].source).segments.empty()) << "before the stream starts";
}

TEST(DashManifest, KeepsOnlyTheMostRecentSegmentsOfAnEndlessLiveStream)
{
    // One-second segments since 2000 and no time-shift window: hundreds of millions of segments.
    const MediaOffer offer = ParseOk(SingleTrack("type=\"dynamic\" availabilityStartTime=\"2000-01-01T00:00:00Z\"",
                                                 "<SegmentTemplate duration=\"1\" media=\"$Number$.m4s\"/>"));
    const TrackSegments& track = Segments(offer.variants[0].source);
    ASSERT_EQ(track.segments.size(), ldm::media::kMaxSegmentsPerTrack);
    const auto completed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        LiveStart() - std::chrono::sys_days(std::chrono::year{2000} / 1 / 1)).count());
    EXPECT_EQ(track.segments.back().sequence, completed) << "startNumber 1, so the last number is the count";
}

TEST(DashManifest, ReadsALiveTimelineUpToNow)
{
    const MediaOffer offer = ParseOk(
        SingleTrack("type=\"dynamic\" availabilityStartTime=\"2026-10-24T10:00:00Z\" timeShiftBufferDepth=\"PT10S\"",
                    "<SegmentTemplate timescale=\"10\" media=\"$Time$.m4s\"><SegmentTimeline>"
                    "<S t=\"0\" d=\"20\" r=\"-1\"/></SegmentTimeline></SegmentTemplate>"),
        LiveStart() + std::chrono::seconds(21));
    const TrackSegments& track = Segments(offer.variants[0].source);
    // 10 two-second segments are complete at 21 s; the window keeps those ending after 11 s.
    ASSERT_EQ(track.segments.size(), 5u);
    EXPECT_EQ(track.segments.front().url, "https://media.example.test/vod/movie/100.m4s");
    EXPECT_EQ(track.segments.front().sequence, 6u);
    EXPECT_EQ(track.segments.back().url, "https://media.example.test/vod/movie/180.m4s");
}

TEST(DashManifest, MarksDrmAndKeepsNoSegments)
{
    const MediaOffer offer = ParseOk(ReadFixture("drm.mpd"));
    ASSERT_TRUE(offer.drm);
    EXPECT_EQ(offer.drm->systems, (std::vector<std::string>{"Common Encryption", "Widevine"}));
    ASSERT_EQ(offer.variants.size(), 1u);
    ASSERT_EQ(offer.audio.size(), 1u);
    const TrackSegments& video = Segments(offer.variants[0].source);
    EXPECT_TRUE(video.drm);
    EXPECT_TRUE(video.segments.empty());
    EXPECT_TRUE(video.inits.empty());
    EXPECT_TRUE(Segments(*offer.audio[0].source).segments.empty()) << "the clear audio of a protected stream too";
}

TEST(DashManifest, JoinsPeriodsWithTheSameRepresentations)
{
    const MediaOffer offer = ParseOk(ReadFixture("multi-period.mpd"));
    EXPECT_DOUBLE_EQ(offer.seconds.value_or(0), 10);
    ASSERT_EQ(offer.variants.size(), 1u);
    ASSERT_EQ(offer.audio.size(), 1u);
    const TrackSegments& video = Segments(offer.variants[0].source);
    ASSERT_EQ(video.inits.size(), 2u);
    EXPECT_EQ(video.inits[1].url, "https://media.example.test/vod/movie/part2/v-init.mp4");
    ASSERT_EQ(video.segments.size(), 5u) << "4 s and 6 s in 2 s segments";
    EXPECT_EQ(video.segments[1].url, "https://media.example.test/vod/movie/part1/v-2.m4s");
    EXPECT_FALSE(video.segments[1].discontinuity);
    EXPECT_EQ(video.segments[1].init, 0u);
    EXPECT_EQ(video.segments[2].url, "https://media.example.test/vod/movie/part2/v-1.m4s");
    EXPECT_TRUE(video.segments[2].discontinuity);
    EXPECT_EQ(video.segments[2].init, 1u);
    EXPECT_EQ(Segments(*offer.audio[0].source).segments.size(), 5u);
}

TEST(DashManifest, RefusesPeriodsWithDifferentContent)
{
    const std::string period = "<Period duration=\"PT2S\"><AdaptationSet contentType=\"video\" mimeType=\"video/mp4\">"
                               "<SegmentTemplate duration=\"2\" media=\"$Number$.m4s\"/>"
                               "<Representation id=\"ID\" bandwidth=\"1\"/></AdaptationSet></Period>";
    std::string first = period;
    std::string second = period;
    first.replace(first.find("ID"), 2, "main");
    second.replace(second.find("ID"), 2, "advert");
    const auto offer = Parse("<MPD type=\"static\">" + first + second + "</MPD>");
    ASSERT_FALSE(offer);
    EXPECT_EQ(offer.GetError().code, ldm::base::ErrorCode::Unsupported);
}

TEST(DashManifest, RefusesBrokenManifests)
{
    const std::string templ = "<SegmentTemplate duration=\"2\" media=\"$Number$.m4s\"/>";
    const std::string length = "mediaPresentationDuration=\"PT4S\"";
    EXPECT_NE(Refusal("<NotMPD/>"), "");
    EXPECT_NE(Refusal("<MPD><Period/>"), "") << "not well-formed";
    EXPECT_NE(Refusal("<MPD type=\"static\"/>"), "") << "no period";
    EXPECT_NE(Refusal(SingleTrack("type=\"live\"", templ)), "");
    EXPECT_NE(Refusal(SingleTrack("type=\"dynamic\"", templ)), "") << "no availabilityStartTime";
    EXPECT_NE(Refusal(SingleTrack("type=\"dynamic\" availabilityStartTime=\"yesterday\"", templ)), "");
    EXPECT_NE(Refusal(SingleTrack("mediaPresentationDuration=\"P1M\"", templ)), "") << "months have no length";
    EXPECT_NE(Refusal(SingleTrack("mediaPresentationDuration=\"4s\"", templ)), "");
    EXPECT_NE(Refusal(SingleTrack("", templ)), "") << "a static template needs a known length";
    EXPECT_NE(Refusal(SingleTrack(length, "<SegmentTemplate media=\"$Number$.m4s\"/>")), "") << "no duration";
    EXPECT_NE(Refusal(SingleTrack(length, "<SegmentTemplate duration=\"2\"/>")), "") << "no media";
    EXPECT_NE(Refusal(SingleTrack(length, "<SegmentTemplate duration=\"2\" timescale=\"0\" media=\"a\"/>")), "");
    EXPECT_NE(Refusal(SingleTrack(length, "<SegmentTemplate duration=\"-2\" media=\"a\"/>")), "");
    EXPECT_NE(Refusal(SingleTrack(length, "<SegmentList/>")), "") << "no SegmentURL";
    EXPECT_NE(Refusal(SingleTrack(length, "<SegmentList><SegmentURL mediaRange=\"9-1\"/></SegmentList>")), "");
    EXPECT_NE(Refusal(SingleTrack(length, "<SegmentTemplate media=\"a\"><SegmentTimeline><S t=\"10\" d=\"1\"/>"
                                          "<S t=\"5\" d=\"1\"/></SegmentTimeline></SegmentTemplate>")),
              "")
        << "back in time";
    EXPECT_NE(Refusal(SingleTrack(length, "<SegmentTemplate media=\"a\"><SegmentTimeline><S d=\"0\"/>"
                                          "</SegmentTimeline></SegmentTemplate>")),
              "");
    EXPECT_NE(Refusal(SingleTrack(length, "<SegmentTemplate media=\"a\"><SegmentTimeline><S d=\"1\" r=\"-2\"/>"
                                          "</SegmentTimeline></SegmentTemplate>")),
              "");
    EXPECT_NE(Refusal("<MPD " + length + "><Period><AdaptationSet contentType=\"video\">" + templ +
                      "<Representation bandwidth=\"1\"/></AdaptationSet></Period></MPD>"),
              "")
        << "no id";
    EXPECT_NE(Refusal("<MPD " + length + "><Period><AdaptationSet contentType=\"video\">" + templ +
                      "<Representation id=\"v\" bandwidth=\"1\"/><Representation id=\"v\" bandwidth=\"2\"/>"
                      "</AdaptationSet></Period></MPD>"),
              "")
        << "duplicate id";
}

TEST(DashManifest, RefusesTemplatesItCannotFillIn)
{
    const std::string length = "mediaPresentationDuration=\"PT4S\"";
    for (const char* media : {"$Unknown$.m4s", "$Number.m4s", "$Number%5d$.m4s", "$Number%099d$.m4s",
                              "$RepresentationID%05d$.m4s", "$Number%0xd$.m4s"}) {
        EXPECT_NE(Refusal(SingleTrack(length, std::string("<SegmentTemplate duration=\"2\" media=\"") + media + "\"/>")),
                  "")
            << media;
    }
    EXPECT_NE(Refusal(SingleTrack(length, "<SegmentTemplate duration=\"2\" media=\"a\" initialization=\"$Number$\"/>")),
              "")
        << "an init section has no number";
    const MediaOffer dollars =
        ParseOk(SingleTrack(length, "<SegmentTemplate duration=\"4\" media=\"a$$b-$Number%03d$.m4s\"/>"));
    EXPECT_EQ(Segments(dollars.variants[0].source).segments[0].url, "https://media.example.test/vod/movie/a$b-001.m4s");
}

TEST(DashManifest, RefusesAddressesThatAreNotHttp)
{
    const std::string length = "mediaPresentationDuration=\"PT4S\"";
    EXPECT_NE(Refusal("<MPD " + length + "><BaseURL>file:///C:/</BaseURL><Period><AdaptationSet contentType=\"video\">"
                                         "<Representation id=\"v\" bandwidth=\"1\"/></AdaptationSet></Period></MPD>"),
              "");
    EXPECT_NE(Refusal(SingleTrack(length, "<SegmentTemplate duration=\"2\" media=\"file:///C:/$Number$.m4s\"/>")), "");
    EXPECT_NE(Refusal(SingleTrack(length, "<SegmentList><SegmentURL media=\"javascript:alert(1)\"/></SegmentList>")),
              "");
    const auto root = ldm::platform::ReadXml(SingleTrack(length, "<SegmentTemplate duration=\"2\" media=\"a\"/>"));
    ASSERT_TRUE(root);
    EXPECT_EQ(ldm::media::ParseMpd(root.Value(), "file:///C:/a.mpd", LiveStart()).GetError().code,
              ldm::base::ErrorCode::InvalidArgument);
}

TEST(DashManifest, RefusesExplosiveSegmentCountsBeforeMakingThem)
{
    // A million hours in one-second segments, and a timeline repeating a trillion times.
    EXPECT_NE(Refusal(SingleTrack("mediaPresentationDuration=\"PT1000000H\"",
                                  "<SegmentTemplate duration=\"1\" media=\"$Number$.m4s\"/>")),
              "");
    EXPECT_NE(Refusal(SingleTrack("mediaPresentationDuration=\"PT4S\"",
                                  "<SegmentTemplate media=\"$Time$.m4s\"><SegmentTimeline>"
                                  "<S d=\"1\" r=\"999999999999\"/></SegmentTimeline></SegmentTemplate>")),
              "");
    EXPECT_NE(Refusal(SingleTrack("mediaPresentationDuration=\"PT4S\"",
                                  "<SegmentTemplate media=\"$Time$.m4s\"><SegmentTimeline>"
                                  "<S d=\"1\" r=\"18446744073709551615\"/></SegmentTimeline></SegmentTemplate>")),
              "");
}

}  // namespace
