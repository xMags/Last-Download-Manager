#include "platform/win/FfmpegCommands.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using ldm::core::MuxInput;
using ldm::core::MuxPiece;
using ldm::platform::MediaContainer;
namespace fs = std::filesystem;

const fs::path kParts = LR"(C:\Downloads\Clip.mp4.ldmparts)";

MuxInput Track(ldm::model::TrackRole role, std::string codecs)
{
    MuxInput input;
    input.role = role;
    input.codecs = std::move(codecs);
    return input;
}

TEST(FfmpegCommands, Mp4UnlessACodecNeedsMatroska)
{
    EXPECT_EQ(ldm::platform::ContainerFor({Track(ldm::model::TrackRole::Video, "avc1.64001f,mp4a.40.2")}), MediaContainer::Mp4);
    EXPECT_EQ(ldm::platform::ContainerFor({Track(ldm::model::TrackRole::Video, "vp09.00.10.08"),
                                           Track(ldm::model::TrackRole::Audio, "opus")}),
              MediaContainer::Mp4);
    EXPECT_EQ(ldm::platform::ContainerFor({Track(ldm::model::TrackRole::Video, ""), Track(ldm::model::TrackRole::Audio, "")}),
              MediaContainer::Mp4) << "unknown codecs try MP4 first";
    EXPECT_EQ(ldm::platform::ContainerFor({Track(ldm::model::TrackRole::Video, "vp8"), Track(ldm::model::TrackRole::Audio, "vorbis")}),
              MediaContainer::Matroska);
    EXPECT_EQ(ldm::platform::ContainerFor({Track(ldm::model::TrackRole::Audio, " Vorbis ")}), MediaContainer::Matroska);
}

TEST(FfmpegCommands, AudioOnlyFilesHaveAudioExtensions)
{
    const std::vector<MuxInput> video{Track(ldm::model::TrackRole::Video, ""), Track(ldm::model::TrackRole::Audio, "")};
    const std::vector<MuxInput> audio{Track(ldm::model::TrackRole::Audio, "")};
    EXPECT_EQ(ldm::platform::ExtensionFor(MediaContainer::Mp4, video), "mp4");
    EXPECT_EQ(ldm::platform::ExtensionFor(MediaContainer::Matroska, video), "mkv");
    EXPECT_EQ(ldm::platform::ExtensionFor(MediaContainer::Mp4, audio), "m4a");
    EXPECT_EQ(ldm::platform::ExtensionFor(MediaContainer::Matroska, audio), "mka");
}

TEST(FfmpegCommands, LocalPlaylistKeepsInitSectionsDurationsAndDiscontinuities)
{
    MuxInput input = Track(ldm::model::TrackRole::Video, "");
    input.pieces = {
        MuxPiece{kParts / L"t0" / L"000000.mp4", true, 0, false, std::nullopt},
        MuxPiece{kParts / L"t0" / L"000001.m4s", false, 4'000'000, false, 0u},
        MuxPiece{kParts / L"t0" / L"000002.m4s", false, 3'500'000, false, 0u},
        MuxPiece{kParts / L"t0" / L"000003.mp4", true, 0, false, std::nullopt},
        MuxPiece{kParts / L"t0" / L"000004.m4s", false, 0, true, 3u},
    };
    const auto playlist = ldm::platform::LocalPlaylist(input, kParts);
    ASSERT_TRUE(playlist);
    EXPECT_EQ(*playlist,
              "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-PLAYLIST-TYPE:VOD\n#EXT-X-TARGETDURATION:4\n#EXT-X-MEDIA-SEQUENCE:0\n"
              "#EXT-X-MAP:URI=\"t0/000000.mp4\"\n"
              "#EXTINF:4.000000,\nt0/000001.m4s\n"
              "#EXTINF:3.500000,\nt0/000002.m4s\n"
              "#EXT-X-DISCONTINUITY\n#EXT-X-MAP:URI=\"t0/000003.mp4\"\n"
              "#EXTINF:0.001000,\nt0/000004.m4s\n"
              "#EXT-X-ENDLIST\n");
}

TEST(FfmpegCommands, LocalPlaylistRefusesFilesOutsideTheFolder)
{
    MuxInput input = Track(ldm::model::TrackRole::Video, "");
    input.pieces = {MuxPiece{LR"(C:\Windows\win.ini)", false, 1'000'000, false, std::nullopt}};
    EXPECT_FALSE(ldm::platform::LocalPlaylist(input, kParts));
    input.pieces = {MuxPiece{kParts / L"t0" / L"00000\x00e9.ts", false, 1'000'000, false, std::nullopt}};
    EXPECT_FALSE(ldm::platform::LocalPlaylist(input, kParts)) << "LDM's own names are ASCII";
}

TEST(FfmpegCommands, JoinReadsLocalFilesOnlyAndCopiesWithoutReencoding)
{
    const std::vector<ldm::platform::MuxSource> sources{
        {kParts / L"track0.m3u8", true, ldm::model::TrackRole::Video},
        {kParts / L"t1.mp4", false, ldm::model::TrackRole::Audio},
    };
    const auto arguments = ldm::platform::JoinArguments(sources, MediaContainer::Mp4, kParts / L"output.mp4");
    const auto has = [&arguments](const std::vector<std::wstring>& run) {
        return std::search(arguments.begin(), arguments.end(), run.begin(), run.end()) != arguments.end();
    };
    EXPECT_TRUE(has({L"-protocol_whitelist", L"file,crypto"}));
    EXPECT_TRUE(has({L"-extension_picky", L"0", L"-i", (kParts / L"track0.m3u8").native()}));
    EXPECT_TRUE(has({L"-i", (kParts / L"t1.mp4").native(), L"-map", L"0:v?", L"-map", L"0:a?", L"-map", L"1:a?"}));
    EXPECT_TRUE(has({L"-c", L"copy"}));
    EXPECT_EQ(arguments.back(), (kParts / L"output.mp4").native());
    const auto whole = std::count(arguments.begin(), arguments.end(), L"-extension_picky");
    EXPECT_EQ(whole, 1) << "only the playlist input gets the playlist options";
}

TEST(FfmpegCommands, ProbeOutputBecomesADescription)
{
    const std::string json = R"({
        "streams": [
            {"codec_type": "audio", "codec_name": "aac", "channels": 2, "sample_rate": "48000"},
            {"codec_type": "video", "codec_name": "h264", "width": 1280, "height": 720, "avg_frame_rate": "30000/1001"},
            {"codec_type": "audio", "codec_name": "ac3", "channels": 6, "sample_rate": "44100"}
        ],
        "format": {"duration": "245.4"}
    })";
    EXPECT_EQ(ldm::platform::DescribeProbe(json), "1280x720, 29.97 fps, H.264; AAC, stereo, 48 kHz; 4:05");
    EXPECT_EQ(ldm::platform::DescribeProbe(R"({"streams":[{"codec_type":"video","codec_name":"vp9","width":320,
        "height":240,"avg_frame_rate":"25/1"}],"format":{"duration":"3725"}})"),
              "320x240, 25 fps, VP9; 1:02:05");
    EXPECT_EQ(ldm::platform::DescribeProbe("not json"), "");
    EXPECT_EQ(ldm::platform::DescribeProbe(R"({"streams":"x"})"), "");
    EXPECT_EQ(ldm::platform::DescribeProbe(R"({"streams":[{"codec_type":7,"width":"wide"}]})"), "");
}

}  // namespace
