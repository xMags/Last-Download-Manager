#include "MediaFixtures.h"

#include "base/Utf.h"
#include "platform/win/ChildProcess.h"

#include <chrono>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace ldm::test {

namespace {

// The test picture and the silent audio every fixture starts from, encoded with FFmpeg's built-in
// encoders. Filter sources need no input device, so the build can leave libavdevice out; and the
// silent source produces AAC's sample format directly, as the build has no libswresample to
// convert a tone's.
std::vector<std::wstring> Source()
{
    return {L"-filter_complex",
            L"testsrc2=size=320x240:rate=25:duration=4[v];anullsrc=channel_layout=stereo:sample_rate=48000:duration=4[a]",
            L"-map", L"[v]", L"-map", L"[a]",
            L"-c:v", L"mpeg4", L"-q:v", L"5", L"-g", L"25",
            L"-c:a", L"aac", L"-b:a", L"64k"};
}

// Runs ffmpeg in `folder`, so the relative names in its arguments land there.
base::Status RunFfmpeg(const std::filesystem::path& folder, std::vector<std::wstring> arguments)
{
    platform::ChildProcessRequest request;
    request.program = FfmpegExe();
    request.arguments = {L"-hide_banner", L"-loglevel", L"error", L"-nostdin", L"-y"};
    request.arguments.insert(request.arguments.end(), arguments.begin(), arguments.end());
    request.workingDirectory = folder;
    request.limits.timeout = std::chrono::seconds(60);
    const auto result = platform::RunChildProcess(request);
    if (!result) {
        return result.GetError();
    }
    if (result.Value().end != platform::ChildProcessEnd::Exited || result.Value().exitCode != 0) {
        return base::MakeError(base::ErrorCode::Unknown,
                               "ffmpeg failed in " + base::ToUtf8(folder.filename().native()) + ": " + result.Value().output);
    }
    return {};
}

base::Result<std::filesystem::path> MakeFolder(const std::filesystem::path& parent, const wchar_t* name)
{
    std::error_code error;
    const auto folder = parent / name;
    std::filesystem::create_directories(folder, error);
    if (error) {
        return base::MakeError(base::ErrorCode::Io, "cannot create " + base::ToUtf8(folder.native()) + ": " + error.message());
    }
    return folder;
}

bool WriteText(const std::filesystem::path& file, const std::string& text)
{
    std::ofstream out(file, std::ios::binary);
    out << text;
    return static_cast<bool>(out);
}

}  // namespace

// Builds every fixture; stops at the first failure.
base::Result<MediaFixturePaths> MakeMediaFixtures(const std::filesystem::path& root)
{
    MediaFixturePaths paths;

    {
        const auto folder = MakeFolder(root, L"hls-ts");
        if (!folder) {
            return folder.GetError();
        }
        auto arguments = Source();
        arguments.insert(arguments.end(), {L"-f", L"hls", L"-hls_time", L"1", L"-hls_playlist_type", L"vod",
                                           L"-hls_segment_filename", L"seg%03d.ts", L"index.m3u8"});
        if (const auto made = RunFfmpeg(folder.Value(), arguments); !made) {
            return made.GetError();
        }
        paths.hlsTs = folder.Value() / L"index.m3u8";
    }
    {
        const auto folder = MakeFolder(root, L"hls-fmp4");
        if (!folder) {
            return folder.GetError();
        }
        auto arguments = Source();
        arguments.insert(arguments.end(), {L"-f", L"hls", L"-hls_time", L"1", L"-hls_playlist_type", L"vod",
                                           L"-hls_segment_type", L"fmp4", L"-hls_fmp4_init_filename", L"init.mp4",
                                           L"-hls_segment_filename", L"seg%03d.m4s", L"index.m3u8"});
        if (const auto made = RunFfmpeg(folder.Value(), arguments); !made) {
            return made.GetError();
        }
        paths.hlsFmp4 = folder.Value() / L"index.m3u8";
    }
    {
        const auto folder = MakeFolder(root, L"hls-byterange");
        if (!folder) {
            return folder.GetError();
        }
        auto arguments = Source();
        arguments.insert(arguments.end(), {L"-f", L"hls", L"-hls_time", L"1", L"-hls_playlist_type", L"vod",
                                           L"-hls_flags", L"single_file", L"index.m3u8"});
        if (const auto made = RunFfmpeg(folder.Value(), arguments); !made) {
            return made.GetError();
        }
        paths.hlsByteRange = folder.Value() / L"index.m3u8";
    }
    {
        const auto folder = MakeFolder(root, L"hls-aes");
        if (!folder) {
            return folder.GetError();
        }
        // A fixed key and IV, so a test can decrypt a segment and compare.
        std::string key;
        for (char i = 0; i < 16; ++i) {
            key.push_back(i);
        }
        const auto keyFile = folder.Value() / L"key.bin";
        const auto keyInfo = folder.Value() / L"keyinfo.txt";
        if (!WriteText(keyFile, key) ||
            !WriteText(keyInfo, "key.bin\n" + base::ToUtf8(keyFile.native()) + "\n000102030405060708090a0b0c0d0e0f\n")) {
            return base::MakeError(base::ErrorCode::Io, "cannot write the AES-128 key files");
        }
        auto arguments = Source();
        arguments.insert(arguments.end(), {L"-f", L"hls", L"-hls_time", L"1", L"-hls_playlist_type", L"vod",
                                           L"-hls_key_info_file", L"keyinfo.txt",
                                           L"-hls_segment_filename", L"seg%03d.ts", L"index.m3u8"});
        if (const auto made = RunFfmpeg(folder.Value(), arguments); !made) {
            return made.GetError();
        }
        paths.hlsAes = folder.Value() / L"index.m3u8";
        paths.hlsAesKey = keyFile;
    }
    {
        const auto folder = MakeFolder(root, L"hls-master");
        if (!folder) {
            return folder.GetError();
        }
        if (!WriteText(folder.Value() / L"subs.vtt",
                       "WEBVTT\n\n00:00:00.000 --> 00:00:02.000\nFirst cue\n\n00:00:02.000 --> 00:00:04.000\nSecond cue\n")) {
            return base::MakeError(base::ErrorCode::Io, "cannot write the subtitle source");
        }
        std::vector<std::wstring> arguments = {L"-i", L"subs.vtt"};
        const auto source = Source();
        arguments.insert(arguments.end(), source.begin(), source.end());
        // Video and subtitles in one variant, the audio as a separate rendition of group "aud".
        arguments.insert(arguments.end(), {L"-map", L"0:s", L"-c:s", L"webvtt",
                                           L"-f", L"hls", L"-hls_time", L"1", L"-hls_playlist_type", L"vod",
                                           L"-master_pl_name", L"master.m3u8",
                                           L"-var_stream_map", L"v:0,agroup:aud,s:0,sgroup:subs a:0,agroup:aud",
                                           L"-hls_segment_filename", L"stream_%v/seg%03d.ts", L"stream_%v/index.m3u8"});
        if (const auto made = RunFfmpeg(folder.Value(), arguments); !made) {
            return made.GetError();
        }
        paths.hlsMaster = folder.Value() / L"master.m3u8";
    }
    {
        const auto folder = MakeFolder(root, L"dash");
        if (!folder) {
            return folder.GetError();
        }
        auto arguments = Source();
        arguments.insert(arguments.end(), {L"-f", L"dash", L"-seg_duration", L"1",
                                           L"-adaptation_sets", L"id=0,streams=v id=1,streams=a", L"manifest.mpd"});
        if (const auto made = RunFfmpeg(folder.Value(), arguments); !made) {
            return made.GetError();
        }
        paths.dash = folder.Value() / L"manifest.mpd";
    }
    return paths;
}

std::filesystem::path FfmpegExe()
{
    return std::filesystem::path(LDM_FFMPEG_EXE);
}

std::filesystem::path FfprobeExe()
{
    return std::filesystem::path(LDM_FFPROBE_EXE);
}

}  // namespace ldm::test
