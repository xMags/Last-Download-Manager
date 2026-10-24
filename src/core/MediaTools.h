#pragma once

#include "core/TransferEngine.h"
#include "model/Ids.h"
#include "model/Media.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// The port to FFmpeg (M3 design section 6): joins a finished stream's pieces into one file and
// describes what it holds. Implemented in platform/win with ffmpeg.exe and ffprobe.exe on a
// worker thread; the core only builds the request.
namespace ldm::core {

// One piece file of a track, in playback order.
struct MuxPiece {
    std::filesystem::path file;
    bool init = false;  // an init section, which the media pieces after it refer to
    std::int64_t durationUs = 0;
    bool discontinuity = false;
    std::optional<std::uint32_t> initPiece;  // index into MuxInput::pieces of its init section
};

struct MuxInput {
    model::TrackRole role = model::TrackRole::Video;
    std::string codecs;  // RFC 6381 list from the manifest; may be empty
    std::string language;
    std::string name;
    // Either the pieces, which FFmpeg reads through a local playlist, or one whole file.
    std::vector<MuxPiece> pieces;
    std::optional<std::filesystem::path> wholeFile;
};

struct MuxRequest {
    model::DownloadId id;
    std::int64_t epoch = 0;
    std::filesystem::path folder;  // the parts folder; the playlists and the output are written here
    std::vector<MuxInput> inputs;  // video and audio, or one subtitle track
    // "mp4" for video and audio (MKV when MP4 cannot hold the streams), "vtt" for subtitles.
    std::string preferredExtension;
};

// Joins and describes. Every method is thread-safe and returns at once; each Mux is answered by
// exactly one MuxFinished event through the engine listener, with the request's id and epoch.
class MediaTools {
public:
    virtual ~MediaTools() = default;
    virtual void Mux(MuxRequest request) = 0;
    // Stops a running Mux; it is then answered with `stopped` set. Nothing happens when it
    // already answered.
    virtual void Cancel(model::DownloadId id, std::int64_t epoch) = 0;
};

}  // namespace ldm::core
