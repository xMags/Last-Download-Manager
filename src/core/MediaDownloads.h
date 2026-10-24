#pragma once

#include "core/MediaPieces.h"
#include "core/MediaProgress.h"
#include "core/MediaTools.h"
#include "core/Ports.h"
#include "core/TransferEngine.h"
#include "model/Download.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The parts of a stream download's life that differ from a file's (M3 design section 6), kept out
// of the coordinator, which decides when they happen. Everything here runs on the core thread.
namespace ldm::core {

// The extension of the parts folder that holds a stream's pieces while it downloads.
inline constexpr std::string_view kPartsFolderExtension = ".ldmparts";

// The tracks whose lists are not known yet: HLS media playlists to fetch before the first run.
[[nodiscard]] std::vector<std::uint32_t> UnlistedTracks(const model::Download& download);

// Reads a fetched HLS media playlist into `track`. Fails, with the reason to show, for a master
// playlist or protected content. A live playlist makes `track` a live recording.
[[nodiscard]] std::optional<TransferFailure> ListTrack(model::MediaTrack& track, const FetchFinished& fetched);

// A live recording's playlist read again: adds what is new to `track` (AppendTrack).
struct TrackReload {
    std::optional<TransferFailure> failure;  // the playlist could not be read; `track` is unchanged
    TrackAppend append;
};
[[nodiscard]] TrackReload ReloadTrack(model::MediaTrack& track, const FetchFinished& fetched);

// True while a track of the stream is a live recording.
[[nodiscard]] bool IsRecording(const model::Download& download);

// Gives up on the pieces of `track` that are not on disk (MediaPiece::skipped): a live
// recording's pieces left over from an earlier run, gone from the stream by now, or those not
// fetched when the recording was finished. Returns whether anything changed.
bool SkipUnfetchedPieces(model::MediaTrack& track);

// For a stream from a public host: the first address of a track, key or piece that names a
// loopback or private host (core::IsPrivateHost). The engine checks the addresses it connects to;
// this refuses plain cases before anything is requested, with a clear reason.
[[nodiscard]] std::optional<TransferFailure> CheckPrivateAddresses(const model::Download& download);

// The engine request of a run: the pieces not on disk yet, and the whole-file tracks with their
// ranges. Connection limits and credentials are the caller's.
[[nodiscard]] MediaRequest BuildMediaRequest(const model::Download& download, const std::filesystem::path& parts);
// What a reload added to track `track` of a running live recording, for the engine.
[[nodiscard]] MediaTrackExtension BuildTrackExtension(const model::Download& download,
                                                      const std::filesystem::path& parts, std::uint32_t track,
                                                      const TrackAppend& append);

// Applies what a run reported. Returns the pieces newly marked done, to store.
std::vector<MediaPieceDone> ApplyPiecesDone(model::Download& download, std::span<const MediaPieceDone> done);
void ApplyRanges(model::Download& download, std::span<const MediaTrackRanges> ranges);

// Forgets the stream's progress: HLS tracks are read again at the next start, DASH pieces are
// marked missing. Returns the tracks changed, to store.
std::vector<std::uint32_t> ResetMediaProgress(model::Download& download);

// After a restart: a piece whose file is gone or has another size than recorded is missing
// again, and so are a whole-file track's ranges when its file is shorter than they say. Returns
// the tracks changed, to store.
std::vector<std::uint32_t> VerifyMediaProgress(model::Download& download, const std::filesystem::path& parts,
                                               FileSystem& fileSystem);

// What to join once every piece is on disk. Skipped pieces are left out, and so are media pieces
// whose init section was skipped; the piece after a gap starts a new timeline (a discontinuity).
[[nodiscard]] MuxRequest BuildMuxRequest(const model::Download& download, const std::filesystem::path& parts);

// How far a stream is, for the list (it has no total size until it is joined).
// Skipped pieces count neither as done nor as to do.
struct StreamSummary {
    int piecesDone = 0;
    int piecesTotal = 0;  // 0 while a track's list is unknown
    // 0..1: by the estimated bytes of each track once every track has some on disk, so a small
    // audio track that finished first does not count as half the stream; until then by duration,
    // each track alike.
    double fraction = 0;
    std::int64_t bytesDone = 0;
    // The stream's size before joining, from each track's bytes and progress; nothing until every
    // track has enough on disk to tell.
    std::optional<std::int64_t> estimatedBytes;
    bool live = false;  // a live recording, whose total is not known
    // The length on disk: the done media pieces of the first video track, or of the first track.
    std::int64_t recordedUs = 0;
};
[[nodiscard]] StreamSummary SummarizeStream(const model::Download& download);

// `name` with its extension replaced by `extension`: "Video.mp4" and "mkv" give "Video.mkv".
[[nodiscard]] std::string WithExtension(const std::string& name, const std::string& extension);

}  // namespace ldm::core
