#include "core/MediaPieces.h"

#include "core/FileNames.h"
#include "core/Url.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace ldm::core {

namespace {

std::optional<std::int64_t> ToInt64(std::uint64_t value)
{
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(value);
}

std::int64_t ToMicroseconds(double seconds)
{
    if (!std::isfinite(seconds) || seconds <= 0) {
        return 0;
    }
    constexpr double kLongest = 1e12;  // eleven days in microseconds: more than any segment
    return static_cast<std::int64_t>(std::llround(std::min(seconds * 1e6, kLongest)));
}

model::MediaPiece Piece(model::MediaPiece::Kind kind, const std::string& url, const std::optional<media::ByteRange>& range)
{
    model::MediaPiece piece;
    piece.kind = kind;
    piece.url = url;
    if (range) {
        const auto offset = ToInt64(range->offset);
        const auto length = ToInt64(range->length);
        if (offset && length && *length > 0 && *offset <= std::numeric_limits<std::int64_t>::max() - *length) {
            piece.rangeOffset = offset;
            piece.rangeLength = length;
        }
    }
    return piece;
}

std::string ExtensionFor(const model::MediaTrack& owner, const model::MediaPiece& piece)
{
    if (piece.kind == model::MediaPiece::Kind::Init) {
        return "mp4";
    }
    if (const auto url = ParseUrl(piece.url)) {
        std::string extension = FileExtension(FileNameFromUrlPath(url->path));
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
        const bool simple = !extension.empty() && extension.size() <= 6 &&
                            std::all_of(extension.begin(), extension.end(), [](char c) {
                                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
                            });
        // A playlist or page extension says nothing about the bytes; those stay generic.
        if (simple && extension != "m3u8" && extension != "mpd" && extension != "php" && extension != "html") {
            return extension;
        }
    }
    return owner.role == model::TrackRole::Subtitles ? "vtt" : "ts";
}

}  // namespace

void FillTrack(model::MediaTrack& track, const media::TrackSegments& segments)
{
    track.keys.clear();
    track.pieces.clear();
    track.ranges.clear();
    for (const media::EncryptionKey& key : segments.keys) {
        track.keys.push_back(model::MediaKey{key.url, key.value});
    }
    const auto& list = segments.segments;
    track.wholeFile = segments.inits.empty() && list.size() == 1 && !list.front().range && !list.front().key;
    for (const media::InitSection& init : segments.inits) {
        track.pieces.push_back(Piece(model::MediaPiece::Kind::Init, init.url, init.range));
    }
    // Init sections come first, so a segment's init index is already its piece index.
    for (const media::Segment& segment : list) {
        model::MediaPiece piece = Piece(model::MediaPiece::Kind::Media, segment.url, segment.range);
        piece.durationUs = ToMicroseconds(segment.seconds);
        piece.sequence = segment.sequence;
        piece.discontinuity = segment.discontinuity;
        if (segment.init && *segment.init < segments.inits.size()) {
            piece.init = static_cast<std::uint32_t>(*segment.init);
        }
        if (segment.key && *segment.key < segments.keys.size()) {
            piece.key = static_cast<std::uint32_t>(*segment.key);
            piece.iv = segment.iv;
        }
        track.pieces.push_back(std::move(piece));
    }
}

std::filesystem::path PiecePath(const std::filesystem::path& parts, std::uint32_t track,
                                const model::MediaTrack& owner, std::uint32_t index)
{
    char name[32] = {};
    std::snprintf(name, sizeof(name), "%06u.", static_cast<unsigned>(index));
    return parts / ("t" + std::to_string(track)) / (name + ExtensionFor(owner, owner.pieces.at(index)));
}

std::filesystem::path WholeFilePath(const std::filesystem::path& parts, std::uint32_t track,
                                    const model::MediaTrack& owner)
{
    const std::string extension = owner.pieces.empty() ? std::string("mp4") : ExtensionFor(owner, owner.pieces.front());
    return parts / ("t" + std::to_string(track) + "." + extension);
}

double TrackSeconds(const model::MediaTrack& track)
{
    std::int64_t total = 0;
    for (const model::MediaPiece& piece : track.pieces) {
        total += piece.durationUs;
    }
    return static_cast<double>(total) / 1e6;
}

}  // namespace ldm::core
