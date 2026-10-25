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

// What AppendTrack added to a track.
struct TrackAppend {
    std::uint32_t firstNewKey = 0;    // the track's key count before the call
    std::uint32_t firstNewPiece = 0;  // the track's piece count before the call
    bool changed = false;             // keys or pieces were added, or `live` changed
};

// Adds to a live recording's track what its reloaded playlist lists after the track's last
// segment (RFC 8216 section 6.3.4): segments with a higher sequence number, with the init sections
// and keys they need that the track does not have yet. Marks a discontinuity on the first new
// piece when segments were missed in between (LDM fell behind the stream's window, or was paused),
// and when the playlist started its numbering over, in which case everything it lists is new.
// Takes over `live`; a track that reaches media::kMaxSegmentsPerTrack pieces stops being live.
TrackAppend AppendTrack(model::MediaTrack& track, const media::TrackSegments& reloaded);

// A site video's track listed again from fresh addresses (`fresh`) takes over what `previous`
// had on disk when both list the same pieces: as many, of the same kinds, lengths, ranges, init
// sections and keys. Only the addresses may differ. Returns whether it did; otherwise `fresh`
// starts from nothing, so bytes of two different lists are never joined.
bool KeepProgress(model::MediaTrack& fresh, const model::MediaTrack& previous);

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
