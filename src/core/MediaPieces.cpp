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
    // A live playlist may list one segment now and more later: it is never one file.
    track.live = segments.live;
    track.wholeFile =
        !segments.live && segments.inits.empty() && list.size() == 1 && !list.front().range && !list.front().key;
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

TrackAppend AppendTrack(model::MediaTrack& track, const media::TrackSegments& reloaded)
{
    TrackAppend append;
    append.firstNewKey = static_cast<std::uint32_t>(track.keys.size());
    append.firstNewPiece = static_cast<std::uint32_t>(track.pieces.size());
    const bool wasLive = track.live;

    const auto lastMedia = std::find_if(track.pieces.rbegin(), track.pieces.rend(), [](const model::MediaPiece& piece) {
        return piece.kind == model::MediaPiece::Kind::Media;
    });
    const auto& listed = reloaded.segments;
    size_t first = 0;  // the first listed segment that is new
    bool gap = false;
    if (lastMedia != track.pieces.rend() && !listed.empty()) {
        const std::uint64_t last = lastMedia->sequence;
        first = static_cast<size_t>(
            std::find_if(listed.begin(), listed.end(), [last](const media::Segment& s) { return s.sequence > last; }) -
            listed.begin());
        if (first < listed.size()) {
            gap = listed[first].sequence != last + 1;
        } else if (listed.back().sequence < last) {
            // Every number is lower: a server that started over, or an older copy of the playlist
            // from a cache. The copy lists segments LDM already has; a fresh start does not.
            constexpr size_t kRecent = 256;
            const size_t from = track.pieces.size() > kRecent ? track.pieces.size() - kRecent : 0;
            const bool known = std::any_of(listed.begin(), listed.end(), [&track, from](const media::Segment& s) {
                return std::any_of(track.pieces.begin() + static_cast<std::ptrdiff_t>(from), track.pieces.end(),
                                   [&s](const model::MediaPiece& piece) { return piece.url == s.url; });
            });
            if (!known) {
                first = 0;
                gap = true;
            }
        }
    }

    // The reloaded playlist's own init and key indexes, mapped to the track's as they are needed.
    std::vector<std::optional<std::uint32_t>> inits(reloaded.inits.size());
    std::vector<std::optional<std::uint32_t>> keys(reloaded.keys.size());
    const auto initIndex = [&track, &reloaded, &inits](size_t index) -> std::uint32_t {
        if (inits[index]) {
            return *inits[index];
        }
        const media::InitSection& init = reloaded.inits[index];
        const model::MediaPiece wanted = Piece(model::MediaPiece::Kind::Init, init.url, init.range);
        for (std::uint32_t p = 0; p < track.pieces.size(); ++p) {
            const model::MediaPiece& piece = track.pieces[p];
            if (piece.kind == model::MediaPiece::Kind::Init && piece.url == wanted.url &&
                piece.rangeOffset == wanted.rangeOffset && piece.rangeLength == wanted.rangeLength) {
                inits[index] = p;
                return p;
            }
        }
        track.pieces.push_back(wanted);
        inits[index] = static_cast<std::uint32_t>(track.pieces.size() - 1);
        return *inits[index];
    };
    const auto keyIndex = [&track, &reloaded, &keys](size_t index) -> std::uint32_t {
        if (keys[index]) {
            return *keys[index];
        }
        const media::EncryptionKey& key = reloaded.keys[index];
        for (std::uint32_t k = 0; k < track.keys.size(); ++k) {
            const model::MediaKey& known = track.keys[k];
            if (key.value ? known.value == key.value : (!known.value && known.url == key.url)) {
                keys[index] = k;
                return k;
            }
        }
        track.keys.push_back(model::MediaKey{key.url, key.value});
        keys[index] = static_cast<std::uint32_t>(track.keys.size() - 1);
        return *keys[index];
    };

    bool full = false;
    for (size_t i = first; i < listed.size(); ++i) {
        // An init section and its segment take two places at most.
        if (track.pieces.size() + 2 > media::kMaxSegmentsPerTrack) {
            full = true;
            break;
        }
        const media::Segment& segment = listed[i];
        model::MediaPiece piece = Piece(model::MediaPiece::Kind::Media, segment.url, segment.range);
        piece.durationUs = ToMicroseconds(segment.seconds);
        piece.sequence = segment.sequence;
        piece.discontinuity = segment.discontinuity || (i == first && gap);
        if (segment.init && *segment.init < reloaded.inits.size()) {
            piece.init = initIndex(*segment.init);
        }
        if (segment.key && *segment.key < reloaded.keys.size()) {
            piece.key = keyIndex(*segment.key);
            piece.iv = segment.iv;
        }
        track.pieces.push_back(std::move(piece));
    }
    track.live = reloaded.live && !full;
    append.changed = track.pieces.size() != append.firstNewPiece || track.keys.size() != append.firstNewKey ||
                     track.live != wasLive;
    return append;
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
