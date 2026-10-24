#pragma once

#include "media/MediaModel.h"
#include "model/Media.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

// How a stream's tracks become files on disk (M3 design section 6): one file per piece in the
// download's parts folder, named by track and position, which FFmpeg later reads through a local
// playlist LDM writes. Pure; the coordinator and the media tools both use it.
namespace ldm::core {

// A track's download list as LDM stores it: its keys, then its init sections followed by its
// segments in order, with the segments' init and key references renumbered to match. A track
// that is one file (a DASH SegmentBase track: one segment without a range or init section) gets
// `wholeFile` and that one piece. Durations become microseconds.
void FillTrack(model::MediaTrack& track, const media::TrackSegments& segments);

// The parts folder's file for a piece: "t<track>\<index>.<extension>". The extension is the
// address's when it is a short alphanumeric one and the piece is not an init section; otherwise
// "mp4" for init sections, "vtt" for subtitles and "ts" for the rest. FFmpeg reads the files by
// their content, so the name only has to be stable.
[[nodiscard]] std::filesystem::path PiecePath(const std::filesystem::path& parts, std::uint32_t track,
                                              const model::MediaTrack& owner, std::uint32_t index);
// The file of a whole-file track: "t<track>.<extension>".
[[nodiscard]] std::filesystem::path WholeFilePath(const std::filesystem::path& parts, std::uint32_t track,
                                                  const model::MediaTrack& owner);

// The total length of a track, from its media pieces.
[[nodiscard]] double TrackSeconds(const model::MediaTrack& track);

}  // namespace ldm::core
