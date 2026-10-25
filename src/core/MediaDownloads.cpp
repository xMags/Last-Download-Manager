#include "core/MediaDownloads.h"

#include "core/MediaPieces.h"
#include "core/NetworkAddress.h"
#include "core/Url.h"
#include "media/HlsPlaylist.h"

#include <algorithm>
#include <variant>

namespace ldm::core {

namespace {

TransferFailure Unsupported(std::string message)
{
    return TransferFailure{FailureReason::UnsupportedStream, FailureClass::NeedsUser, 0, std::move(message)};
}

bool NamesPrivateHost(const std::string& address)
{
    const auto url = ParseUrl(address);
    return !url || IsPrivateHost(url->host);  // an address LDM cannot read is refused, not guessed at
}

std::int64_t RangesEnd(const std::vector<model::Segment>& ranges)
{
    std::int64_t end = 0;
    for (const model::Segment& range : ranges) {
        end = std::max(end, range.end.value_or(range.start + range.done));
    }
    return end;
}

std::int64_t RangesDone(const std::vector<model::Segment>& ranges)
{
    std::int64_t done = 0;
    for (const model::Segment& range : ranges) {
        done += range.done;
    }
    return done;
}

std::int64_t RangesProgressEnd(const std::vector<model::Segment>& ranges)
{
    std::int64_t end = 0;
    for (const model::Segment& range : ranges) {
        end = std::max(end, range.start + range.done);
    }
    return end;
}

}  // namespace

std::vector<std::uint32_t> UnlistedTracks(const model::Download& download)
{
    std::vector<std::uint32_t> unlisted;
    for (std::uint32_t t = 0; t < download.tracks.size(); ++t) {
        if (!download.tracks[t].Listed()) {
            unlisted.push_back(t);
        }
    }
    return unlisted;
}

std::optional<TransferFailure> ListTrack(model::MediaTrack& track, const FetchFinished& fetched)
{
    const std::string& base = fetched.finalUrl.empty() ? track.sourceUrl : fetched.finalUrl;
    auto parsed = media::ParseHls(fetched.body, base);
    if (!parsed) {
        return TransferFailure{FailureReason::Protocol, FailureClass::Fatal, 0,
                               "the stream's playlist could not be read: " + parsed.GetError().message};
    }
    const auto* segments = std::get_if<media::TrackSegments>(&parsed.Value());
    if (segments == nullptr) {
        return TransferFailure{FailureReason::Protocol, FailureClass::Fatal, 0,
                               "the track's address leads to a list of qualities, not to one track"};
    }
    if (segments->drm) {
        return Unsupported("the stream is protected (DRM), which LDM does not download");
    }
    // A live playlist may list nothing yet (RFC 8216 section 6.2.1); its reloads bring segments.
    if (segments->segments.empty() && !segments->live) {
        return TransferFailure{FailureReason::Protocol, FailureClass::Fatal, 0, "the stream's playlist lists no segments"};
    }
    FillTrack(track, *segments);
    return std::nullopt;
}

TrackReload ReloadTrack(model::MediaTrack& track, const FetchFinished& fetched)
{
    TrackReload reload;
    const std::string& base = fetched.finalUrl.empty() ? track.sourceUrl : fetched.finalUrl;
    auto parsed = media::ParseHls(fetched.body, base);
    if (!parsed) {
        reload.failure = TransferFailure{FailureReason::Protocol, FailureClass::Fatal, 0,
                                         "the stream's playlist could not be read: " + parsed.GetError().message};
        return reload;
    }
    const auto* segments = std::get_if<media::TrackSegments>(&parsed.Value());
    if (segments == nullptr) {
        reload.failure = TransferFailure{FailureReason::Protocol, FailureClass::Fatal, 0,
                                         "the track's address now leads to a list of qualities"};
        return reload;
    }
    if (segments->drm) {
        reload.failure = Unsupported("the stream became protected (DRM), which LDM does not download");
        return reload;
    }
    reload.append = AppendTrack(track, *segments);
    return reload;
}

bool IsRecording(const model::Download& download)
{
    return std::any_of(download.tracks.begin(), download.tracks.end(), [](const model::MediaTrack& t) { return t.live; });
}

bool SkipUnfetchedPieces(model::MediaTrack& track)
{
    bool changed = false;
    for (model::MediaPiece& piece : track.pieces) {
        if (!piece.done && !piece.skipped) {
            piece.skipped = true;
            changed = true;
        }
    }
    return changed;
}

std::optional<TransferFailure> CheckPrivateAddresses(const model::Download& download)
{
    if (download.mediaFromPrivateNetwork) {
        return std::nullopt;
    }
    const auto refuse = [](const std::string& address) {
        return TransferFailure{FailureReason::PrivateAddress, FailureClass::Fatal, 0,
                               "the stream comes from the internet but lists " + address.substr(0, 200)};
    };
    for (const model::MediaTrack& track : download.tracks) {
        if (NamesPrivateHost(track.sourceUrl)) {
            return refuse(track.sourceUrl);
        }
        for (const model::MediaKey& key : track.keys) {
            if (!key.value && NamesPrivateHost(key.url)) {
                return refuse(key.url);
            }
        }
        for (const model::MediaPiece& piece : track.pieces) {
            if (NamesPrivateHost(piece.url)) {
                return refuse(piece.url);
            }
        }
    }
    return std::nullopt;
}

namespace {

MediaPieceRequest PieceRequest(const std::filesystem::path& parts, std::uint32_t t, const model::MediaTrack& track,
                               std::uint32_t i)
{
    const model::MediaPiece& piece = track.pieces[i];
    MediaPieceRequest request;
    request.index = i;
    request.url = piece.url;
    request.rangeOffset = piece.rangeOffset;
    request.rangeLength = piece.rangeLength;
    request.key = piece.key;
    request.iv = piece.iv.value_or(model::AesKey{});
    request.path = PiecePath(parts, t, track, i);
    return request;
}

}  // namespace

MediaRequest BuildMediaRequest(const model::Download& download, const std::filesystem::path& parts)
{
    MediaRequest request;
    request.id = download.id;
    request.epoch = download.epoch;
    request.refusePrivateAddresses = !download.mediaFromPrivateNetwork;
    for (std::uint32_t t = 0; t < download.tracks.size(); ++t) {
        const model::MediaTrack& track = download.tracks[t];
        MediaTrackRequest trackRequest;
        trackRequest.track = t;
        if (track.wholeFile) {
            trackRequest.wholeFile = true;
            trackRequest.url = track.pieces.empty() ? track.sourceUrl : track.pieces.front().url;
            trackRequest.path = WholeFilePath(parts, t, track);
            trackRequest.ranges = track.ranges;
            if (download.siteVideo) {
                trackRequest.maxRequestBytes = kSiteVideoRequestBytes;
            }
            request.tracks.push_back(std::move(trackRequest));
            continue;
        }
        trackRequest.keys = track.keys;
        trackRequest.live = track.live;
        for (std::uint32_t i = 0; i < track.pieces.size(); ++i) {
            const model::MediaPiece& piece = track.pieces[i];
            if (!piece.done && !piece.skipped) {
                trackRequest.pieces.push_back(PieceRequest(parts, t, track, i));
            }
        }
        request.tracks.push_back(std::move(trackRequest));
    }
    return request;
}

MediaTrackExtension BuildTrackExtension(const model::Download& download, const std::filesystem::path& parts,
                                        std::uint32_t track, const TrackAppend& append)
{
    MediaTrackExtension extension;
    extension.track = track;
    const model::MediaTrack& owner = download.tracks.at(track);
    extension.live = owner.live;
    for (size_t k = append.firstNewKey; k < owner.keys.size(); ++k) {
        extension.keys.push_back(owner.keys[k]);
    }
    for (std::uint32_t i = append.firstNewPiece; i < owner.pieces.size(); ++i) {
        const model::MediaPiece& piece = owner.pieces[i];
        if (!piece.done && !piece.skipped) {
            extension.pieces.push_back(PieceRequest(parts, track, owner, i));
        }
    }
    return extension;
}

std::vector<MediaPieceDone> ApplyPiecesDone(model::Download& download, std::span<const MediaPieceDone> done)
{
    std::vector<MediaPieceDone> applied;
    for (const MediaPieceDone& piece : done) {
        if (piece.track >= download.tracks.size()) {
            continue;
        }
        model::MediaTrack& track = download.tracks[piece.track];
        if (piece.index >= track.pieces.size() || track.wholeFile) {
            continue;
        }
        model::MediaPiece& stored = track.pieces[piece.index];
        stored.done = true;
        stored.bytes = piece.bytes;
        applied.push_back(piece);
    }
    return applied;
}

void ApplyRanges(model::Download& download, std::span<const MediaTrackRanges> ranges)
{
    for (const MediaTrackRanges& reported : ranges) {
        if (reported.track < download.tracks.size() && download.tracks[reported.track].wholeFile) {
            download.tracks[reported.track].ranges = reported.ranges;
        }
    }
}

std::vector<std::uint32_t> ResetMediaProgress(model::Download& download)
{
    std::vector<std::uint32_t> changed;
    for (std::uint32_t t = 0; t < download.tracks.size(); ++t) {
        model::MediaTrack& track = download.tracks[t];
        track.ranges.clear();
        if (download.kind == model::DownloadKind::Hls) {
            // Read again at the next start: a playlist's addresses may have expired meanwhile.
            track.keys.clear();
            track.pieces.clear();
            track.wholeFile = false;
            track.live = false;
        } else {
            for (model::MediaPiece& piece : track.pieces) {
                piece.done = false;
                piece.bytes.reset();
                piece.skipped = false;
            }
        }
        changed.push_back(t);
    }
    return changed;
}

std::vector<std::uint32_t> VerifyMediaProgress(model::Download& download, const std::filesystem::path& parts,
                                               FileSystem& fileSystem)
{
    std::vector<std::uint32_t> changed;
    for (std::uint32_t t = 0; t < download.tracks.size(); ++t) {
        model::MediaTrack& track = download.tracks[t];
        bool trackChanged = false;
        if (track.wholeFile) {
            const std::int64_t needed = RangesProgressEnd(track.ranges);
            if (needed > 0) {
                const auto size = fileSystem.FileSize(WholeFilePath(parts, t, track));
                if (!size || *size < needed) {
                    track.ranges.clear();
                    trackChanged = true;
                }
            }
        } else {
            for (std::uint32_t i = 0; i < track.pieces.size(); ++i) {
                model::MediaPiece& piece = track.pieces[i];
                if (!piece.done) {
                    continue;
                }
                const auto size = fileSystem.FileSize(PiecePath(parts, t, track, i));
                if (!size || !piece.bytes || *size != *piece.bytes) {
                    piece.done = false;
                    piece.bytes.reset();
                    trackChanged = true;
                }
            }
        }
        if (trackChanged) {
            changed.push_back(t);
        }
    }
    return changed;
}

MuxRequest BuildMuxRequest(const model::Download& download, const std::filesystem::path& parts)
{
    MuxRequest request;
    request.id = download.id;
    request.epoch = download.epoch;
    request.folder = parts;
    const bool onlySubtitles = std::all_of(download.tracks.begin(), download.tracks.end(), [](const model::MediaTrack& t) {
        return t.role == model::TrackRole::Subtitles;
    });
    request.preferredExtension = onlySubtitles ? "vtt" : "mp4";
    for (std::uint32_t t = 0; t < download.tracks.size(); ++t) {
        const model::MediaTrack& track = download.tracks[t];
        MuxInput input;
        input.role = track.role;
        input.codecs = track.codecs;
        input.language = track.language;
        input.name = track.name;
        if (track.wholeFile) {
            input.wholeFile = WholeFilePath(parts, t, track);
        } else {
            // The place in input.pieces of each piece of the track that is kept.
            std::vector<std::optional<std::uint32_t>> kept(track.pieces.size());
            bool gap = false;  // media pieces were left out since the last one kept
            for (std::uint32_t i = 0; i < track.pieces.size(); ++i) {
                const model::MediaPiece& piece = track.pieces[i];
                const bool init = piece.kind == model::MediaPiece::Kind::Init;
                const bool initMissing = piece.init && (*piece.init >= kept.size() || !kept[*piece.init]);
                if (piece.skipped || (!init && initMissing)) {
                    gap = gap || !init;
                    continue;
                }
                MuxPiece muxPiece;
                muxPiece.file = PiecePath(parts, t, track, i);
                muxPiece.init = init;
                muxPiece.durationUs = piece.durationUs;
                muxPiece.discontinuity = !init && (piece.discontinuity || gap);
                if (piece.init) {
                    muxPiece.initPiece = kept[*piece.init];
                }
                if (!init) {
                    gap = false;
                }
                kept[i] = static_cast<std::uint32_t>(input.pieces.size());
                input.pieces.push_back(std::move(muxPiece));
            }
        }
        request.inputs.push_back(std::move(input));
    }
    return request;
}

StreamSummary SummarizeStream(const model::Download& download)
{
    StreamSummary summary;
    summary.live = IsRecording(download);
    const auto video = std::find_if(download.tracks.begin(), download.tracks.end(), [](const model::MediaTrack& t) {
        return t.role == model::TrackRole::Video;
    });
    const model::MediaTrack* measured = video != download.tracks.end() ? &*video
                                        : download.tracks.empty()     ? nullptr
                                                                      : &download.tracks.front();
    // Below this much of a track on disk, its bytes say too little about its size.
    constexpr double kEnoughToEstimate = 0.02;
    double fractions = 0;
    bool allListed = true;
    double estimatedTotal = 0;  // of every track, while each one can be estimated
    bool allEstimated = !download.tracks.empty();
    const auto estimate = [&](std::int64_t bytes, double fraction) {
        if (fraction >= kEnoughToEstimate && bytes > 0) {
            estimatedTotal += static_cast<double>(bytes) / std::min(fraction, 1.0);
        } else {
            allEstimated = false;
        }
    };
    for (const model::MediaTrack& track : download.tracks) {
        if (track.wholeFile) {
            const std::int64_t end = RangesEnd(track.ranges);
            const std::int64_t done = RangesDone(track.ranges);
            summary.piecesTotal += 1;
            summary.bytesDone += done;
            const bool complete = end > 0 && done >= end;
            summary.piecesDone += complete ? 1 : 0;
            const double fraction = end > 0 ? static_cast<double>(done) / static_cast<double>(end) : 0.0;
            fractions += fraction;
            if (end > 0) {
                estimatedTotal += static_cast<double>(end);  // its size is known
            } else {
                allEstimated = false;
            }
            continue;
        }
        if (!track.Listed()) {
            allListed = false;
            allEstimated = false;
            continue;
        }
        std::int64_t totalUs = 0;
        std::int64_t doneUs = 0;
        std::int64_t trackBytes = 0;
        int done = 0;
        int wanted = 0;
        for (const model::MediaPiece& piece : track.pieces) {
            if (piece.skipped) {
                continue;
            }
            ++wanted;
            totalUs += piece.durationUs;
            if (piece.done) {
                ++done;
                doneUs += piece.durationUs;
                trackBytes += piece.bytes.value_or(0);
            }
        }
        if (&track == measured) {
            summary.recordedUs = doneUs;
        }
        summary.bytesDone += trackBytes;
        summary.piecesDone += done;
        summary.piecesTotal += wanted;
        const double fraction = wanted == 0 ? 0.0
                                : totalUs > 0 ? static_cast<double>(doneUs) / static_cast<double>(totalUs)
                                              : static_cast<double>(done) / static_cast<double>(wanted);
        fractions += fraction;
        estimate(trackBytes, fraction);
    }
    if (!allListed) {
        summary.piecesTotal = 0;
    }
    if (allEstimated && estimatedTotal > 0) {
        summary.estimatedBytes = static_cast<std::int64_t>(estimatedTotal);
        summary.fraction = std::clamp(static_cast<double>(summary.bytesDone) / estimatedTotal, 0.0, 1.0);
    } else {
        summary.fraction = download.tracks.empty() ? 0.0
                                                   : std::clamp(fractions / static_cast<double>(download.tracks.size()), 0.0, 1.0);
    }
    return summary;
}

std::string WithExtension(const std::string& name, const std::string& extension)
{
    const size_t dot = name.find_last_of('.');
    const std::string stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
    return stem + "." + extension;
}

}  // namespace ldm::core
