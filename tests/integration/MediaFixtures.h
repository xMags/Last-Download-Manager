#pragma once

#include "base/Result.h"

#include <filesystem>
#include <string>

// Media test fixtures (M3 design section 8), made at test time by the ffmpeg.exe that ships next
// to ldm.exe, so no binary media is committed. Every fixture is 4 seconds of a test picture
// (MPEG-4 Part 2, 320x240, 25 frames a second, a key frame every second) and silent stereo AAC,
// cut into 1-second segments: FFmpeg's built-in encoders, as the LGPL build has no H.264 encoder.
namespace ldm::test {

inline constexpr double kFixtureSeconds = 4.0;

// What each fixture is, relative to the folder it was made in.
struct MediaFixturePaths {
    std::filesystem::path hlsTs;          // VOD media playlist with MPEG-TS segments
    std::filesystem::path hlsFmp4;        // fMP4 segments with an EXT-X-MAP init section
    std::filesystem::path hlsByteRange;   // one .ts file addressed by EXT-X-BYTERANGE
    std::filesystem::path hlsAes;         // MPEG-TS segments encrypted with AES-128
    std::filesystem::path hlsAesKey;      // the 16-byte key for hlsAes
    std::filesystem::path hlsMaster;      // master playlist: video, separate audio, WebVTT subtitles
    std::filesystem::path dash;           // static MPD, video and audio in separate adaptation sets
};

// Makes every fixture under `folder`, which must exist and be empty.
[[nodiscard]] base::Result<MediaFixturePaths> MakeMediaFixtures(const std::filesystem::path& folder);

[[nodiscard]] std::filesystem::path FfmpegExe();
[[nodiscard]] std::filesystem::path FfprobeExe();

}  // namespace ldm::test
