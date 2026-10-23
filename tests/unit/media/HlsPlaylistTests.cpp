#include "media/HlsPlaylist.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <variant>

namespace {

using ldm::media::ByteRange;
using ldm::media::HlsPlaylist;
using ldm::media::MediaOffer;
using ldm::media::ParseHls;
using ldm::media::PlaylistAddress;
using ldm::media::TrackSegments;

constexpr char kBase[] = "https://media.example.test/show/ep1/index.m3u8";

std::string ReadFixture(const char* name)
{
    std::ifstream in(std::filesystem::path(LDM_MEDIA_FIXTURES) / "hls" / name, std::ios::binary);
    EXPECT_TRUE(in) << name;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

MediaOffer ParseMaster(const std::string& text, const char* url = kBase)
{
    auto parsed = ParseHls(text, url);
    EXPECT_TRUE(parsed) << (parsed ? "" : parsed.GetError().message);
    if (!parsed || !std::holds_alternative<MediaOffer>(parsed.Value())) {
        ADD_FAILURE() << "not a master playlist";
        return {};
    }
    return std::get<MediaOffer>(std::move(parsed).Value());
}

TrackSegments ParseMedia(const std::string& text, const char* url = kBase)
{
    auto parsed = ParseHls(text, url);
    EXPECT_TRUE(parsed) << (parsed ? "" : parsed.GetError().message);
    if (!parsed || !std::holds_alternative<TrackSegments>(parsed.Value())) {
        ADD_FAILURE() << "not a media playlist";
        return {};
    }
    return std::get<TrackSegments>(std::move(parsed).Value());
}

std::string PlaylistUrl(const ldm::media::SegmentSource& source)
{
    const auto* address = std::get_if<PlaylistAddress>(&source);
    return address ? address->url : std::string("(not an address)");
}

// The error of a playlist that must be refused, or a failure when it was accepted.
std::string Refusal(const std::string& text, const char* url = kBase)
{
    const auto parsed = ParseHls(text, url);
    if (parsed) {
        ADD_FAILURE() << "accepted:\n" << text;
        return {};
    }
    return parsed.GetError().message;
}

TEST(HlsPlaylist, ReadsAMasterPlaylist)
{
    const MediaOffer offer = ParseMaster(ReadFixture("master.m3u8"), "https://media.example.test/show/ep1/master.m3u8");
    EXPECT_EQ(offer.format, ldm::media::StreamFormat::Hls);
    EXPECT_FALSE(offer.drm);
    EXPECT_FALSE(offer.live);

    ASSERT_EQ(offer.variants.size(), 3u) << "the I-frame playlist is left out";
    const auto& hd = offer.variants[0];
    EXPECT_EQ(hd.bandwidth, 5'000'000u);
    EXPECT_EQ(hd.averageBandwidth, 4'500'000u);
    ASSERT_TRUE(hd.resolution);
    EXPECT_EQ(hd.resolution->width, 1920);
    EXPECT_EQ(hd.resolution->height, 1080);
    EXPECT_DOUBLE_EQ(hd.frameRate.value_or(0), 29.97);
    EXPECT_EQ(hd.codecs, "avc1.640028,mp4a.40.2");
    EXPECT_EQ(hd.audioGroup, "aac");
    EXPECT_EQ(hd.subtitleGroup, "subs");
    EXPECT_EQ(PlaylistUrl(hd.source), "https://media.example.test/show/ep1/1080p/index.m3u8");

    const auto& sd = offer.variants[1];
    EXPECT_EQ(sd.audioGroup, "muxed");
    EXPECT_EQ(sd.subtitleGroup, "") << "a group the playlist does not define is dropped";
    EXPECT_EQ(PlaylistUrl(sd.source), "https://cdn.example.test/480p/index.m3u8?token=abc");

    const auto& audioOnly = offer.variants[2];
    EXPECT_FALSE(audioOnly.resolution);
    EXPECT_EQ(PlaylistUrl(audioOnly.source), "https://media.example.test/show/audio-only/index.m3u8");

    ASSERT_EQ(offer.audio.size(), 3u);
    EXPECT_EQ(offer.audio[0].name, "English");
    EXPECT_EQ(offer.audio[0].language, "en");
    EXPECT_TRUE(offer.audio[0].isDefault);
    EXPECT_EQ(offer.audio[0].channels, 2);
    ASSERT_TRUE(offer.audio[0].source);
    EXPECT_EQ(PlaylistUrl(*offer.audio[0].source), "https://media.example.test/show/ep1/audio/en/index.m3u8");
    EXPECT_EQ(offer.audio[1].channels, 16) << "CHANNELS=\"16/JOC\"";
    EXPECT_FALSE(offer.audio[1].isDefault);
    EXPECT_FALSE(offer.audio[2].source) << "no URI: the audio is inside the variant";

    ASSERT_EQ(offer.subtitles.size(), 2u) << "closed captions are left out";
    EXPECT_FALSE(offer.subtitles[0].forced);
    EXPECT_TRUE(offer.subtitles[1].forced);
    ASSERT_TRUE(offer.subtitles[1].source);
    EXPECT_EQ(PlaylistUrl(*offer.subtitles[1].source), "https://media.example.test/show/ep1/subs/en-forced.m3u8");
}

TEST(HlsPlaylist, ReadsAVodMediaPlaylist)
{
    const TrackSegments track = ParseMedia(ReadFixture("vod-ts.m3u8"));
    EXPECT_FALSE(track.live);
    EXPECT_FALSE(track.drm);
    EXPECT_DOUBLE_EQ(track.targetSeconds, 6);
    EXPECT_TRUE(track.inits.empty());
    EXPECT_TRUE(track.keys.empty());
    ASSERT_EQ(track.segments.size(), 3u);
    EXPECT_EQ(track.segments[0].url, "https://media.example.test/show/ep1/seg100.ts");
    EXPECT_EQ(track.segments[0].sequence, 100u);
    EXPECT_DOUBLE_EQ(track.segments[0].seconds, 6);
    EXPECT_FALSE(track.segments[0].discontinuity);
    EXPECT_FALSE(track.segments[0].range);
    EXPECT_FALSE(track.segments[0].key);
    EXPECT_EQ(track.segments[0].iv, (std::array<std::uint8_t, 16>{}));
    EXPECT_DOUBLE_EQ(track.segments[1].seconds, 5.5) << "the title after the comma is ignored";
    EXPECT_EQ(track.segments[2].url, "https://media.example.test/other/seg102.ts");
    EXPECT_EQ(track.segments[2].sequence, 102u);
    EXPECT_TRUE(track.segments[2].discontinuity);
    EXPECT_DOUBLE_EQ(ldm::media::TotalSeconds(track), 15.75);
}

TEST(HlsPlaylist, ReadsInitSectionsAndByteRanges)
{
    const TrackSegments track = ParseMedia(ReadFixture("fmp4-byterange.m3u8"));
    ASSERT_EQ(track.inits.size(), 2u);
    EXPECT_EQ(track.inits[0].url, "https://media.example.test/show/ep1/video.mp4");
    ASSERT_TRUE(track.inits[0].range);
    EXPECT_EQ(track.inits[0].range->offset, 0u);
    EXPECT_EQ(track.inits[0].range->length, 720u);
    EXPECT_FALSE(track.inits[1].range);

    ASSERT_EQ(track.segments.size(), 4u);
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 3> ranges{{{720, 1000}, {1720, 1200}, {2920, 800}}};
    for (size_t i = 0; i < ranges.size(); ++i) {
        ASSERT_TRUE(track.segments[i].range) << i;
        EXPECT_EQ(track.segments[i].range->offset, ranges[i].first) << i;
        EXPECT_EQ(track.segments[i].range->length, ranges[i].second) << i;
        EXPECT_EQ(track.segments[i].init, 0u) << i;
    }
    EXPECT_TRUE(track.segments[3].discontinuity);
    EXPECT_EQ(track.segments[3].init, 1u);
    EXPECT_FALSE(track.segments[3].range);
    EXPECT_EQ(track.segments[3].url, "https://media.example.test/show/ep1/next/part1.m4s");
}

TEST(HlsPlaylist, ReadsAes128KeysAndWorksOutTheIvs)
{
    const TrackSegments track = ParseMedia(ReadFixture("aes.m3u8"));
    EXPECT_FALSE(track.drm);
    ASSERT_EQ(track.keys.size(), 3u);
    EXPECT_EQ(track.keys[0].url, "https://keys.example.test/k1?id=1");
    EXPECT_EQ(track.keys[1].url, "https://media.example.test/show/ep1/key2.bin");
    EXPECT_TRUE(track.keys[2].url.empty());
    ASSERT_TRUE(track.keys[2].value);
    EXPECT_EQ(*track.keys[2].value,
              (std::array<std::uint8_t, 16>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}));

    ASSERT_EQ(track.segments.size(), 5u);
    EXPECT_EQ(track.segments[0].key, 0u);
    EXPECT_EQ(track.segments[0].iv,
              (std::array<std::uint8_t, 16>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}));
    // Without IV, the IV is the media sequence number (7 + position), big-endian.
    EXPECT_EQ(track.segments[1].key, 1u);
    EXPECT_EQ(track.segments[1].iv, (std::array<std::uint8_t, 16>{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8}));
    EXPECT_EQ(track.segments[2].iv, (std::array<std::uint8_t, 16>{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9}));
    EXPECT_FALSE(track.segments[3].key) << "METHOD=NONE ends encryption";
    EXPECT_EQ(track.segments[3].iv, (std::array<std::uint8_t, 16>{}));
    EXPECT_EQ(track.segments[4].key, 2u);
    EXPECT_EQ(track.segments[4].iv, (std::array<std::uint8_t, 16>{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 11}));
}

TEST(HlsPlaylist, LargeSequenceNumbersMakeFullWidthIvs)
{
    const TrackSegments track = ParseMedia(
        "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:72623859790382856\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"k\"\n#EXTINF:2,\na.ts\n#EXT-X-ENDLIST\n");
    ASSERT_EQ(track.segments.size(), 1u);
    EXPECT_EQ(track.segments[0].iv,
              (std::array<std::uint8_t, 16>{0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8}));
}

TEST(HlsPlaylist, MarksDrmAndKeepsNoSegments)
{
    const TrackSegments track = ParseMedia(ReadFixture("drm.m3u8"));
    ASSERT_TRUE(track.drm);
    EXPECT_EQ(track.drm->systems, (std::vector<std::string>{"FairPlay", "Widevine"}));
    EXPECT_TRUE(track.segments.empty());
    EXPECT_TRUE(track.keys.empty());
    EXPECT_TRUE(track.inits.empty());

    const TrackSegments sampleAes = ParseMedia(
        "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"k\"\n#EXTINF:6,\na.ts\n#EXT-X-ENDLIST\n");
    ASSERT_TRUE(sampleAes.drm);
    EXPECT_EQ(sampleAes.drm->systems, (std::vector<std::string>{"SAMPLE-AES"}));

    const TrackSegments unknownMethod = ParseMedia(
        "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-KEY:METHOD=AES-256,URI=\"k\"\n#EXTINF:6,\na.ts\n#EXT-X-ENDLIST\n");
    ASSERT_TRUE(unknownMethod.drm) << "a method LDM cannot decrypt is not downloaded";
    EXPECT_TRUE(unknownMethod.segments.empty());

    const MediaOffer offer = ParseMaster(
        "#EXTM3U\n"
        "#EXT-X-SESSION-KEY:METHOD=SAMPLE-AES,URI=\"skd://x\",KEYFORMAT=\"com.apple.streamingkeydelivery\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1000\nv.m3u8\n");
    ASSERT_TRUE(offer.drm);
    EXPECT_EQ(offer.drm->systems, (std::vector<std::string>{"FairPlay"}));

    const MediaOffer clear = ParseMaster(
        "#EXTM3U\n#EXT-X-SESSION-KEY:METHOD=AES-128,URI=\"k\"\n#EXT-X-STREAM-INF:BANDWIDTH=1000\nv.m3u8\n");
    EXPECT_FALSE(clear.drm) << "AES-128 is decrypted by LDM itself";
}

TEST(HlsPlaylist, ReadsALivePlaylistWithCrlfAndAByteOrderMark)
{
    const TrackSegments track = ParseMedia(
        "\xEF\xBB\xBF#EXTM3U\r\n#EXT-X-TARGETDURATION:4\r\n#EXT-X-MEDIA-SEQUENCE:2680\r\n\r\n"
        "#EXTINF:3.96,\r\nfileSequence2680.ts\r\n#EXTINF:3.96,\r\nfileSequence2681.ts\r\n");
    EXPECT_TRUE(track.live) << "no EXT-X-ENDLIST";
    ASSERT_EQ(track.segments.size(), 2u);
    EXPECT_EQ(track.segments[1].url, "https://media.example.test/show/ep1/fileSequence2681.ts");
    EXPECT_EQ(track.segments[1].sequence, 2681u);

    const TrackSegments starting = ParseMedia("#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXT-X-PLAYLIST-TYPE:EVENT\n");
    EXPECT_TRUE(starting.live);
    EXPECT_TRUE(starting.segments.empty()) << "a live playlist may not have segments yet";
}

TEST(HlsPlaylist, RefusesBrokenPlaylists)
{
    const std::string head = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n";
    EXPECT_NE(Refusal("not a playlist\n#EXTM3U\n"), "");
    EXPECT_NE(Refusal("#EXTM3U\n"), "");
    EXPECT_NE(Refusal(head + "#EXT-X-STREAM-INF:BANDWIDTH=1\nv.m3u8\n#EXTINF:4,\na.ts\n"), "") << "mixed kinds";
    EXPECT_NE(Refusal("#EXTM3U\n#EXTINF:4,\na.ts\n#EXT-X-ENDLIST\n"), "") << "no target duration";
    EXPECT_NE(Refusal(head + "a.ts\n"), "") << "address without EXTINF";
    EXPECT_NE(Refusal(head + "#EXTINF:4,\n"), "") << "EXTINF without address";
    EXPECT_NE(Refusal(head + "#EXTINF:4,\n#EXTINF:4,\na.ts\n"), "");
    EXPECT_NE(Refusal(head + "#EXTINF:-4,\na.ts\n"), "");
    EXPECT_NE(Refusal(head + "#EXTINF:abc,\na.ts\n"), "");
    EXPECT_NE(Refusal(head + "#EXTINF:4,\na.ts\n#EXT-X-MEDIA-SEQUENCE:5\n"), "") << "sequence after a segment";
    EXPECT_NE(Refusal(head + "#EXT-X-PLAYLIST-TYPE:LIVE\n"), "");
    EXPECT_NE(Refusal("#EXTM3U\n#EXT-X-STREAM-INF:CODECS=\"a\"\nv.m3u8\n"), "") << "no bandwidth";
    EXPECT_NE(Refusal("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\n"), "") << "variant without address";
    EXPECT_NE(Refusal("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1,RESOLUTION=0x0\nv.m3u8\n"), "");
    EXPECT_NE(Refusal("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1,BANDWIDTH=2\nv.m3u8\n"), "") << "duplicate attribute";
    EXPECT_NE(Refusal("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1,CODECS=\"a\nv.m3u8\n"), "") << "unterminated quote";
    EXPECT_NE(Refusal("#EXTM3U\n#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"s\",NAME=\"x\"\n"
                      "#EXT-X-STREAM-INF:BANDWIDTH=1\nv.m3u8\n"),
              "")
        << "subtitles need a URI";
}

TEST(HlsPlaylist, RefusesByteRangesThatDoNotAddUp)
{
    const std::string head = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n";
    EXPECT_NE(Refusal(head + "#EXTINF:4,\n#EXT-X-BYTERANGE:100\na.ts\n"), "") << "no previous range";
    EXPECT_NE(Refusal(head + "#EXTINF:4,\n#EXT-X-BYTERANGE:100@0\na.ts\n#EXTINF:4,\n#EXT-X-BYTERANGE:100\nb.ts\n"), "")
        << "previous range is of another file";
    EXPECT_NE(Refusal(head + "#EXTINF:4,\nwhole.ts\n#EXTINF:4,\n#EXT-X-BYTERANGE:100\nwhole.ts\n"), "")
        << "previous segment had no range";
    EXPECT_NE(Refusal(head + "#EXTINF:4,\n#EXT-X-BYTERANGE:18446744073709551615@1\na.ts\n"), "") << "overflow";
    EXPECT_NE(Refusal(head + "#EXTINF:4,\n#EXT-X-BYTERANGE:0@0\na.ts\n"), "") << "empty range";
    EXPECT_NE(Refusal(head + "#EXT-X-MAP:URI=\"i.mp4\",BYTERANGE=\"x\"\n"), "");
}

TEST(HlsPlaylist, RefusesBadKeys)
{
    const std::string head = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n";
    EXPECT_NE(Refusal(head + "#EXT-X-KEY:METHOD=AES-128,URI=\"k\",IV=0x0102\n"), "") << "short IV";
    EXPECT_NE(Refusal(head + "#EXT-X-KEY:METHOD=AES-128,URI=\"k\",IV=0x000102030405060708090A0B0C0D0E0G\n"), "");
    EXPECT_NE(Refusal(head + "#EXT-X-KEY:METHOD=AES-128\n"), "") << "no URI";
    EXPECT_NE(Refusal(head + "#EXT-X-KEY:METHOD=AES-128,URI=\"\"\n"), "") << "empty URI: not the playlist itself";
    EXPECT_NE(Refusal(head + "#EXT-X-MAP:URI=\"\"\n"), "") << "empty URI: not the playlist itself";
    EXPECT_NE(Refusal(head + "#EXT-X-KEY:METHOD=AES-128,URI=\"file:///C:/key.bin\"\n"), "");
    EXPECT_NE(Refusal(head + "#EXT-X-KEY:METHOD=AES-128,URI=\"data:text/plain;base64,AAEC\"\n"), "") << "3 bytes";
    EXPECT_NE(Refusal(head + "#EXT-X-KEY:METHOD=AES-128,URI=\"data:text/plain,0123456789abcdef\"\n"), "");
}

TEST(HlsPlaylist, RefusesAddressesThatAreNotHttp)
{
    const std::string head = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n";
    for (const char* address : {"file:///C:/Windows/win.ini", "javascript:alert(1)", "ftp://a.test/a.ts"}) {
        EXPECT_NE(Refusal(head + "#EXTINF:4,\n" + address + "\n"), "") << address;
        EXPECT_NE(Refusal("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\n" + std::string(address) + "\n"), "") << address;
    }
    EXPECT_NE(Refusal(head + "#EXT-X-MAP:URI=\"file:///C:/init.mp4\"\n"), "");
    // A Windows path never comes out as a file address.
    const auto parsed = ParseHls("#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXTINF:4,\nC:\\a.ts\n", kBase);
    EXPECT_FALSE(parsed) << "C: reads as a scheme";
    EXPECT_EQ(ParseHls(head, "file:///C:/index.m3u8").GetError().code, ldm::base::ErrorCode::InvalidArgument);
}

TEST(HlsPlaylist, RefusesOversizedPlaylists)
{
    EXPECT_NE(Refusal(std::string(ldm::media::kMaxManifestBytes + 1, '#')), "");

    std::string many = "#EXTM3U\n#EXT-X-TARGETDURATION:1\n";
    many.reserve(ldm::media::kMaxSegmentsPerTrack * 12 + 100);
    for (size_t i = 0; i <= ldm::media::kMaxSegmentsPerTrack; ++i) {
        many += "#EXTINF:1,\ns\n";
    }
    EXPECT_NE(Refusal(many), "");
}

TEST(HlsPlaylist, NamesTheLineOfAnError)
{
    EXPECT_EQ(Refusal("#EXTM3U\n#EXT-X-TARGETDURATION:4\n\n#EXTINF:4,\nfile:///x.ts\n"),
              "line 5: the segment's address is not an http or https address");
}

}  // namespace
