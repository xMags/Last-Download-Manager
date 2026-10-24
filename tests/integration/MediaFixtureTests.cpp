// The FFmpeg build that ships next to ldm.exe (M3 design section 6) and the media fixtures it makes
// for the HLS and DASH tests: every fixture is checked by duration and stream count with ffprobe,
// and FFmpeg is checked to be LGPL and to refuse network addresses under LDM's protocol list.
// LDM's own HLS and DASH parsers then read what FFmpeg wrote, so the hand-written parser fixtures
// cannot drift from real output unnoticed.
#include "MediaFixtures.h"

#include "base/Utf.h"
#include "media/DashManifest.h"
#include "media/HlsPlaylist.h"
#include "platform/win/ChildProcess.h"
#include "platform/win/XmlLiteReader.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <variant>
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

// The fixtures as if served from this address, so every parsed address maps back to a file.
constexpr std::string_view kFixtureOrigin = "https://fixture.test/";

class ParsedFixtureTest : public MediaFixtureTest {
protected:
    static std::string UrlOf(const std::filesystem::path& file)
    {
        return std::string(kFixtureOrigin) + std::filesystem::relative(file, folder_).generic_string();
    }

    // The fixture file an address parsed from a fixture points at.
    static std::filesystem::path FileOf(const std::string& url)
    {
        EXPECT_TRUE(url.starts_with(kFixtureOrigin)) << url;
        return folder_ / std::filesystem::path(url.substr(std::min(url.size(), kFixtureOrigin.size())));
    }

    static std::string ReadAll(const std::filesystem::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    static ldm::media::HlsPlaylist ParseHlsFile(const std::filesystem::path& file)
    {
        auto parsed = ldm::media::ParseHls(ReadAll(file), UrlOf(file));
        EXPECT_TRUE(parsed) << file.string() << ": " << (parsed ? "" : parsed.GetError().message);
        return parsed ? std::move(parsed).Value() : ldm::media::HlsPlaylist{};
    }

    static ldm::media::TrackSegments ParseHlsMedia(const std::filesystem::path& file)
    {
        auto playlist = ParseHlsFile(file);
        if (!std::holds_alternative<ldm::media::TrackSegments>(playlist)) {
            ADD_FAILURE() << file.string() << " is not a media playlist";
            return {};
        }
        return std::get<ldm::media::TrackSegments>(std::move(playlist));
    }

    // Every address of the track names a fixture file, and the segments last the fixture's length.
    static void ExpectTrackOnDisk(const ldm::media::TrackSegments& track)
    {
        EXPECT_FALSE(track.live);
        EXPECT_FALSE(track.drm);
        ASSERT_GE(track.segments.size(), 3u);
        EXPECT_NEAR(ldm::media::TotalSeconds(track), ldm::test::kFixtureSeconds, 0.5);
        for (const auto& init : track.inits) {
            EXPECT_TRUE(std::filesystem::exists(FileOf(init.url))) << init.url;
        }
        for (const auto& segment : track.segments) {
            const auto file = FileOf(segment.url);
            ASSERT_TRUE(std::filesystem::exists(file)) << segment.url;
            if (segment.range) {
                EXPECT_LE(segment.range->offset + segment.range->length, std::filesystem::file_size(file));
            }
        }
    }
};

TEST_F(ParsedFixtureTest, HlsTransportStreamPlaylist)
{
    const auto track = ParseHlsMedia(paths_->hlsTs);
    ExpectTrackOnDisk(track);
    EXPECT_TRUE(track.inits.empty());
    size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(paths_->hlsTs.parent_path())) {
        files += entry.path().extension() == ".ts" ? 1 : 0;
    }
    EXPECT_EQ(track.segments.size(), files) << "one segment per file FFmpeg wrote";
}

TEST_F(ParsedFixtureTest, HlsFragmentedMp4Playlist)
{
    const auto track = ParseHlsMedia(paths_->hlsFmp4);
    ExpectTrackOnDisk(track);
    ASSERT_EQ(track.inits.size(), 1u);
    EXPECT_EQ(FileOf(track.inits[0].url).filename(), "init.mp4");
    for (const auto& segment : track.segments) {
        EXPECT_EQ(segment.init, 0u);
    }
}

TEST_F(ParsedFixtureTest, HlsByteRangePlaylistCoversItsFile)
{
    const auto track = ParseHlsMedia(paths_->hlsByteRange);
    ExpectTrackOnDisk(track);
    ASSERT_FALSE(track.segments.empty());
    // The ranges follow each other without gaps, up to the file's end.
    std::uint64_t next = track.segments.front().range ? track.segments.front().range->offset : 0;
    for (const auto& segment : track.segments) {
        ASSERT_TRUE(segment.range) << segment.url;
        EXPECT_EQ(segment.url, track.segments.front().url);
        EXPECT_EQ(segment.range->offset, next);
        next = segment.range->offset + segment.range->length;
    }
    EXPECT_EQ(next, std::filesystem::file_size(FileOf(track.segments.front().url)));
}

TEST_F(ParsedFixtureTest, HlsAes128PlaylistNamesItsKeyAndIv)
{
    const auto track = ParseHlsMedia(paths_->hlsAes);
    ExpectTrackOnDisk(track);
    ASSERT_EQ(track.keys.size(), 1u);
    EXPECT_EQ(FileOf(track.keys[0].url), paths_->hlsAesKey);
    const std::array<std::uint8_t, 16> iv{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    for (const auto& segment : track.segments) {
        EXPECT_EQ(segment.key, 0u);
        EXPECT_EQ(segment.iv, iv) << "the IV of the key info file";
    }
}

TEST_F(ParsedFixtureTest, HlsMasterPlaylistAndItsRenditions)
{
    const auto playlist = ParseHlsFile(paths_->hlsMaster);
    ASSERT_TRUE(std::holds_alternative<ldm::media::MediaOffer>(playlist));
    const auto& offer = std::get<ldm::media::MediaOffer>(playlist);
    EXPECT_FALSE(offer.drm);
    ASSERT_GE(offer.variants.size(), 1u);
    ASSERT_EQ(offer.audio.size(), 1u);
    ASSERT_EQ(offer.subtitles.size(), 1u);
    EXPECT_EQ(offer.variants[0].audioGroup, offer.audio[0].groupId);
    EXPECT_EQ(offer.variants[0].subtitleGroup, offer.subtitles[0].groupId);

    const auto mediaPlaylist = [](const ldm::media::SegmentSource& source) {
        const auto* address = std::get_if<ldm::media::PlaylistAddress>(&source);
        EXPECT_TRUE(address);
        return address ? FileOf(address->url) : std::filesystem::path();
    };
    ExpectTrackOnDisk(ParseHlsMedia(mediaPlaylist(offer.variants[0].source)));
    ASSERT_TRUE(offer.audio[0].source);
    ExpectTrackOnDisk(ParseHlsMedia(mediaPlaylist(*offer.audio[0].source)));
    ASSERT_TRUE(offer.subtitles[0].source);
    const auto subtitles = ParseHlsMedia(mediaPlaylist(*offer.subtitles[0].source));
    ASSERT_FALSE(subtitles.segments.empty());
    for (const auto& segment : subtitles.segments) {
        EXPECT_TRUE(std::filesystem::exists(FileOf(segment.url))) << segment.url;
    }
}

TEST_F(ParsedFixtureTest, DashManifest)
{
    auto root = ldm::platform::ReadXml(ReadAll(paths_->dash));
    ASSERT_TRUE(root) << root.GetError().message;
    auto parsed = ldm::media::ParseMpd(root.Value(), UrlOf(paths_->dash), std::chrono::system_clock::now());
    ASSERT_TRUE(parsed) << parsed.GetError().message;
    const auto& offer = parsed.Value();
    EXPECT_FALSE(offer.live);
    EXPECT_NEAR(offer.seconds.value_or(0), ldm::test::kFixtureSeconds, 0.5);
    ASSERT_EQ(offer.variants.size(), 1u);
    ASSERT_EQ(offer.audio.size(), 1u);
    ASSERT_TRUE(offer.audio[0].source);
    for (const ldm::media::SegmentSource* source : {&offer.variants[0].source, &*offer.audio[0].source}) {
        const auto& track = std::get<ldm::media::TrackSegments>(*source);
        ExpectTrackOnDisk(track);
        ASSERT_EQ(track.inits.size(), 1u);
        // One segment per chunk file FFmpeg wrote for this track ("chunk-stream0-00001.m4s").
        const std::string name = FileOf(track.segments.front().url).filename().string();
        const std::string prefix = name.substr(0, name.rfind('-') + 1);
        size_t files = 0;
        for (const auto& entry : std::filesystem::directory_iterator(paths_->dash.parent_path())) {
            files += entry.path().filename().string().starts_with(prefix) ? 1 : 0;
        }
        EXPECT_EQ(track.segments.size(), files) << prefix;
    }
}

}  // namespace
