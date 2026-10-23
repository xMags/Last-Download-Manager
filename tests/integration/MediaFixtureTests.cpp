// The FFmpeg build that ships next to ldm.exe (M3 design section 6) and the media fixtures it makes
// for the HLS and DASH tests: every fixture is checked by duration and stream count with ffprobe,
// and FFmpeg is checked to be LGPL and to refuse network addresses under LDM's protocol list.
#include "MediaFixtures.h"

#include "base/Utf.h"
#include "platform/win/ChildProcess.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace {

using ldm::platform::ChildProcessEnd;
using ldm::platform::ChildProcessRequest;
using ldm::platform::RunChildProcess;
using ldm::test::MediaFixturePaths;

// What every LDM call of FFmpeg passes for its inputs: local files only.
constexpr wchar_t kProtocols[] = L"file,crypto";

struct Probe {
    double seconds = 0;
    std::map<std::string, int> streams;  // codec_type -> count
};

ldm::platform::ChildProcessResult RunTool(const std::filesystem::path& program, std::vector<std::wstring> arguments)
{
    ChildProcessRequest request;
    request.program = program;
    request.arguments = std::move(arguments);
    request.limits.timeout = std::chrono::seconds(60);
    auto result = RunChildProcess(request);
    EXPECT_TRUE(result) << (result ? "" : result.GetError().message);
    return result ? std::move(result).Value() : ldm::platform::ChildProcessResult{};
}

// `inputOptions` go before the input, for example options of FFmpeg's HLS reader.
std::optional<Probe> ProbeFile(const std::filesystem::path& file, const std::vector<std::wstring>& inputOptions = {})
{
    std::vector<std::wstring> arguments = {L"-v", L"error", L"-protocol_whitelist", kProtocols, L"-show_entries",
                                           L"format=duration:stream=codec_type", L"-of", L"json"};
    arguments.insert(arguments.end(), inputOptions.begin(), inputOptions.end());
    arguments.push_back(file.native());
    const auto run = RunTool(ldm::test::FfprobeExe(), std::move(arguments));
    if (run.end != ChildProcessEnd::Exited || run.exitCode != 0) {
        ADD_FAILURE() << "ffprobe failed on " << file.string() << ": " << run.output;
        return std::nullopt;
    }
    const auto json = nlohmann::json::parse(run.output, nullptr, false);
    if (json.is_discarded()) {
        ADD_FAILURE() << "ffprobe printed no JSON: " << run.output;
        return std::nullopt;
    }
    Probe probe;
    probe.seconds = std::stod(json.at("format").at("duration").get<std::string>());
    for (const auto& stream : json.value("streams", nlohmann::json::array())) {
        ++probe.streams[stream.value("codec_type", std::string("?"))];
    }
    return probe;
}

class MediaFixtureTest : public testing::Test {
protected:
    static void SetUpTestSuite()
    {
        folder_ = std::filesystem::temp_directory_path() / (L"ldm-media-" + std::to_wstring(::GetCurrentProcessId()));
        std::error_code ignored;
        std::filesystem::remove_all(folder_, ignored);
        std::filesystem::create_directories(folder_);
        auto made = ldm::test::MakeMediaFixtures(folder_);
        if (made) {
            paths_ = std::move(made).Value();
        } else {
            error_ = made.GetError().message;
        }
    }

    static void TearDownTestSuite()
    {
        std::error_code ignored;
        std::filesystem::remove_all(folder_, ignored);
    }

    void SetUp() override { ASSERT_TRUE(paths_) << "the fixtures could not be made: " << error_; }

    static void ExpectVideoAndAudio(const std::filesystem::path& file, const std::vector<std::wstring>& inputOptions = {})
    {
        const auto probe = ProbeFile(file, inputOptions);
        ASSERT_TRUE(probe);
        EXPECT_NEAR(probe->seconds, ldm::test::kFixtureSeconds, 0.5) << file.string();
        EXPECT_EQ(probe->streams.count("video") ? probe->streams.at("video") : 0, 1) << file.string();
        EXPECT_EQ(probe->streams.count("audio") ? probe->streams.at("audio") : 0, 1) << file.string();
    }

    static inline std::filesystem::path folder_;
    static inline std::optional<MediaFixturePaths> paths_;
    static inline std::string error_;
};

TEST(FfmpegBuildTest, IsLgplAndLinksNoExternalLibraries)
{
    const auto run = RunTool(ldm::test::FfmpegExe(), {L"-hide_banner", L"-version"});
    ASSERT_EQ(run.exitCode, 0u) << run.output;
    const auto configuration = run.output.find("configuration:");
    ASSERT_NE(configuration, std::string::npos) << run.output;
    const std::string line = run.output.substr(configuration, run.output.find('\n', configuration) - configuration);
    EXPECT_NE(line.find("--disable-autodetect"), std::string::npos) << line;
    EXPECT_EQ(line.find("--enable-gpl"), std::string::npos) << line;
    EXPECT_EQ(line.find("--enable-nonfree"), std::string::npos) << line;
    EXPECT_EQ(line.find("--enable-lib"), std::string::npos) << line;
}

TEST(FfmpegBuildTest, RefusesANetworkAddressUnderLdmsProtocolList)
{
    const auto run = RunTool(ldm::test::FfprobeExe(), {L"-v", L"error", L"-protocol_whitelist", kProtocols,
                                                   L"http://127.0.0.1:9/video.m3u8"});
    EXPECT_NE(run.exitCode, 0u);
    EXPECT_NE(run.output.find("whitelist"), std::string::npos) << run.output;
}

// The attack that matters: a local manifest LDM downloaded, pointing at a network address.
TEST(FfmpegBuildTest, RefusesANetworkSegmentInALocalPlaylist)
{
    const auto folder = std::filesystem::temp_directory_path() / (L"ldm-netplaylist-" + std::to_wstring(::GetCurrentProcessId()));
    std::filesystem::create_directories(folder);
    const auto playlist = folder / L"index.m3u8";
    {
        std::ofstream out(playlist, std::ios::binary);
        out << "#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1.0,\nhttp://127.0.0.1:9/seg0.ts\n#EXT-X-ENDLIST\n";
    }
    const auto run = RunTool(ldm::test::FfprobeExe(), {L"-v", L"error", L"-protocol_whitelist", kProtocols, playlist.native()});
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    EXPECT_NE(run.output.find("whitelist"), std::string::npos) << run.output;
}

TEST_F(MediaFixtureTest, HlsWithTransportStreamSegments)
{
    ExpectVideoAndAudio(paths_->hlsTs);
}

TEST_F(MediaFixtureTest, HlsWithFragmentedMp4Segments)
{
    ExpectVideoAndAudio(paths_->hlsFmp4);
    std::ifstream playlist(paths_->hlsFmp4);
    const std::string text((std::istreambuf_iterator<char>(playlist)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("#EXT-X-MAP:URI=\"init.mp4\""), std::string::npos) << text;
}

TEST_F(MediaFixtureTest, HlsWithByteRanges)
{
    ExpectVideoAndAudio(paths_->hlsByteRange);
    std::ifstream playlist(paths_->hlsByteRange);
    const std::string text((std::istreambuf_iterator<char>(playlist)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("#EXT-X-BYTERANGE:"), std::string::npos) << text;
}

TEST_F(MediaFixtureTest, HlsEncryptedWithAes128)
{
    // FFmpeg's HLS reader opens only files with media extensions, the key included. LDM decrypts
    // segments itself and never hands FFmpeg a key; only this check needs the reader to read one.
    ExpectVideoAndAudio(paths_->hlsAes, {L"-allowed_extensions", L"ALL"});
    std::ifstream playlist(paths_->hlsAes);
    const std::string text((std::istreambuf_iterator<char>(playlist)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\""), std::string::npos) << text;
    EXPECT_EQ(std::filesystem::file_size(paths_->hlsAesKey), 16u);
}

TEST_F(MediaFixtureTest, HlsMasterWithSeparateAudioAndSubtitles)
{
    std::ifstream playlist(paths_->hlsMaster);
    const std::string text((std::istreambuf_iterator<char>(playlist)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("TYPE=AUDIO"), std::string::npos) << text;
    EXPECT_NE(text.find("TYPE=SUBTITLES"), std::string::npos) << text;
    EXPECT_NE(text.find("#EXT-X-STREAM-INF:"), std::string::npos) << text;
    const auto probe = ProbeFile(paths_->hlsMaster);
    ASSERT_TRUE(probe);
    EXPECT_NEAR(probe->seconds, ldm::test::kFixtureSeconds, 0.5);
    EXPECT_GE(probe->streams.count("video") ? probe->streams.at("video") : 0, 1);
    EXPECT_GE(probe->streams.count("audio") ? probe->streams.at("audio") : 0, 1);
}

// This FFmpeg build cannot read MPD manifests (its DASH reader needs libxml2, which the build
// leaves out; LDM parses DASH itself). So each track is checked as LDM will hand it to FFmpeg:
// its init segment followed by its media segments, in order, in one file.
TEST_F(MediaFixtureTest, DashWithSeparateVideoAndAudio)
{
    std::ifstream manifest(paths_->dash);
    const std::string text((std::istreambuf_iterator<char>(manifest)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("contentType=\"video\""), std::string::npos) << text;
    EXPECT_NE(text.find("contentType=\"audio\""), std::string::npos) << text;
    EXPECT_NE(text.find("<SegmentTemplate"), std::string::npos) << text;

    const auto folder = paths_->dash.parent_path();
    std::map<std::string, int> streams;
    for (const char* id : {"0", "1"}) {
        std::vector<std::filesystem::path> chunks;
        const std::string prefix = std::string("chunk-stream") + id + "-";
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            if (entry.path().filename().string().starts_with(prefix)) {
                chunks.push_back(entry.path());
            }
        }
        std::sort(chunks.begin(), chunks.end());  // numbered with leading zeros
        ASSERT_GE(chunks.size(), 3u) << "track " << id;
        const auto joined = folder / (std::string("track") + id + ".mp4");
        {
            std::ofstream out(joined, std::ios::binary);
            std::ifstream init(folder / (std::string("init-stream") + id + ".m4s"), std::ios::binary);
            ASSERT_TRUE(init) << "track " << id << " has no init segment";
            out << init.rdbuf();
            for (const auto& part : chunks) {
                std::ifstream in(part, std::ios::binary);
                out << in.rdbuf();
            }
        }
        const auto probe = ProbeFile(joined);
        ASSERT_TRUE(probe);
        EXPECT_NEAR(probe->seconds, ldm::test::kFixtureSeconds, 0.5) << "track " << id;
        for (const auto& [type, count] : probe->streams) {
            streams[type] += count;
        }
    }
    EXPECT_EQ(streams["video"], 1);
    EXPECT_EQ(streams["audio"], 1);
}

}  // namespace
