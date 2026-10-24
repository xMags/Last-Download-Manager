#pragma once

#include "core/MediaTools.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What LDM asks of ffmpeg.exe and ffprobe.exe (M3 design section 6), as text and arguments.
// Pure, so the rules are tested without running FFmpeg; FfmpegMediaTools runs them.
namespace ldm::platform {

// Every FFmpeg call reads local files only.
inline constexpr std::wstring_view kFfmpegProtocols = L"file,crypto";

enum class MediaContainer {
    Mp4,
    Matroska,
};

// MP4 unless a track's codecs are ones MP4 cannot hold (VP8, Vorbis, Theora); unknown or missing
// codecs try MP4 first, and a failed MP4 join is tried again as Matroska.
[[nodiscard]] MediaContainer ContainerFor(const std::vector<core::MuxInput>& inputs);

// The file extension for the joined file: "mp4" or "mkv", and "m4a" or "mka" without video.
[[nodiscard]] std::string ExtensionFor(MediaContainer container, const std::vector<core::MuxInput>& inputs);

// A local HLS media playlist for one track's pieces, with paths relative to `folder` (where the
// playlist is written): durations, discontinuities and init sections (EXT-X-MAP) as the manifest
// gave them, so FFmpeg's HLS reader puts the pieces back together. Nothing when a piece is not
// inside `folder`.
[[nodiscard]] std::optional<std::string> LocalPlaylist(const core::MuxInput& input, const std::filesystem::path& folder);

// One input of a join: a local playlist or a whole file.
struct MuxSource {
    std::filesystem::path file;
    bool playlist = false;
    model::TrackRole role = model::TrackRole::Video;
};

// ffmpeg arguments that copy every video and audio stream of `sources` into `output` without
// re-encoding.
[[nodiscard]] std::vector<std::wstring> JoinArguments(const std::vector<MuxSource>& sources, MediaContainer container,
                                                      const std::filesystem::path& output);
// ffmpeg arguments that turn subtitles read from `source` into a WebVTT file.
[[nodiscard]] std::vector<std::wstring> SubtitleArguments(const MuxSource& source, const std::filesystem::path& output);
// ffprobe arguments that print the streams and the length of `file` as JSON.
[[nodiscard]] std::vector<std::wstring> ProbeArguments(const std::filesystem::path& file);

// The description of a file from ffprobe's JSON (checklist C-04): "1280x720, 30 fps, H.264;
// AAC, stereo, 48 kHz; 4:05". Empty when the output is not ffprobe's JSON.
[[nodiscard]] std::string DescribeProbe(std::string_view json);

}  // namespace ldm::platform
