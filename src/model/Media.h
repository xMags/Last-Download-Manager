#pragma once

#include "model/Segment.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What a download of a streamed video (HLS or DASH) keeps between runs (M3 design section 7):
// the tracks it is made of and, per track, the pieces to fetch and which of them are on disk.
// Filled from a manifest by the core; the model only holds it.
namespace ldm::model {

enum class DownloadKind {
    File,  // one address, one file: every download before streams
    Hls,
    Dash,
};

[[nodiscard]] std::string_view ToString(DownloadKind kind) noexcept;
[[nodiscard]] std::optional<DownloadKind> ParseDownloadKind(std::string_view text) noexcept;

enum class TrackRole {
    Video,  // may carry its own sound (an HLS variant with muxed audio)
    Audio,
    Subtitles,
};

[[nodiscard]] std::string_view ToString(TrackRole role) noexcept;
[[nodiscard]] std::optional<TrackRole> ParseTrackRole(std::string_view text) noexcept;

// How a site video added from a list (a playlist, a channel) picks its quality when it first
// starts: the best picture, the best up to a height, or sound only (video-sites round 2, 2.4).
enum class SiteQuality {
    Best,
    UpTo2160,
    UpTo1440,
    UpTo1080,
    UpTo720,
    UpTo480,
    AudioOnly,
};

[[nodiscard]] std::string_view ToString(SiteQuality quality) noexcept;
[[nodiscard]] std::optional<SiteQuality> ParseSiteQuality(std::string_view text) noexcept;

using AesKey = std::array<std::uint8_t, 16>;

// An AES-128 key: an address to fetch the 16 bytes from, or the bytes a playlist embedded.
struct MediaKey {
    std::string url;  // empty when `value` is set
    std::optional<AesKey> value;
};

// One file of a track: an init section or a media segment.
struct MediaPiece {
    enum class Kind {
        Init,   // the header fMP4 media segments need in front of them (HLS EXT-X-MAP)
        Media,
    };
    Kind kind = Kind::Media;
    std::string url;
    std::optional<std::int64_t> rangeOffset;  // with rangeLength: a part of `url`
    std::optional<std::int64_t> rangeLength;
    std::int64_t durationUs = 0;
    std::uint64_t sequence = 0;
    bool discontinuity = false;
    std::optional<std::uint32_t> init;  // index of an Init piece of the same track
    std::optional<std::uint32_t> key;   // index into MediaTrack::keys; none for clear pieces
    std::optional<AesKey> iv;
    // On disk in full and flushed; `bytes` is then the size of its file.
    bool done = false;
    std::optional<std::int64_t> bytes;
    // Given up on: a live recording's piece that left the stream's window before LDM fetched it,
    // or was not fetched yet when the recording was finished. Never fetched; the join leaves it
    // out and marks the gap. Pieces keep their places, because their files are named by them.
    bool skipped = false;
};

struct MediaTrack {
    TrackRole role = TrackRole::Video;
    // HLS: the media playlist, read at the first start. DASH: the MPD, already read at the add.
    std::string sourceUrl;
    std::string codecs;  // RFC 6381 list as the manifest gave it
    std::string language;
    std::string name;
    // A DASH track that is one file (SegmentBase): downloaded like a file, in byte ranges kept in
    // `ranges`, from the address of its only piece.
    bool wholeFile = false;
    // An HLS live stream still being recorded: its playlist is read again while the download runs
    // and new pieces are added at the end. Off once the stream ended or the user finished it.
    bool live = false;
    // A site video's track (Download::siteVideo): the resolver's format id, to ask for the same
    // format again, and its exact length when the site stated one, to tell the same bytes came.
    std::string siteFormat;
    std::optional<std::int64_t> expectedBytes;
    std::vector<MediaKey> keys;
    // Empty until the track's list is known (an HLS media playlist not read yet).
    std::vector<MediaPiece> pieces;
    std::vector<Segment> ranges;

    // A live track is listed once its playlist was read, even before the stream's first segment.
    [[nodiscard]] bool Listed() const noexcept { return !pieces.empty() || live; }
};

}  // namespace ldm::model
