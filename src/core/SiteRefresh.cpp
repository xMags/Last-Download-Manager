#include "core/SiteRefresh.h"

#include "core/MediaPieces.h"
#include "core/Url.h"

#include <algorithm>
#include <charconv>
#include <variant>

namespace ldm::core {

namespace {

// The format `id` of the video's offer: where its pieces come from, and the length the site states.
struct OfferedFormat {
    const media::SegmentSource* source = nullptr;
    const media::SiteFormat* site = nullptr;
};

std::optional<OfferedFormat> FindFormat(const media::MediaOffer& offer, const std::string& id)
{
    for (const media::Variant& variant : offer.variants) {
        if (variant.site && variant.site->id == id) {
            return OfferedFormat{&variant.source, &*variant.site};
        }
    }
    for (const auto* renditions : {&offer.audio, &offer.subtitles}) {
        for (const media::Rendition& rendition : *renditions) {
            if (rendition.site && rendition.site->id == id && rendition.source) {
                return OfferedFormat{&*rendition.source, &*rendition.site};
            }
        }
    }
    return std::nullopt;
}

// The address of a whole file, from a format that is one.
std::optional<std::string> WholeFileUrl(const OfferedFormat& format)
{
    const auto* track = std::get_if<media::TrackSegments>(format.source);
    if (track == nullptr || track->segments.size() != 1 || !track->inits.empty() || track->segments.front().range) {
        return std::nullopt;
    }
    return track->segments.front().url;
}

// The track's current address: its first piece (a whole-file track's only one). Its sourceUrl is
// the video's page, or for HLS its playlist.
std::string_view AddressOf(const model::MediaTrack& track)
{
    return track.pieces.empty() ? std::string_view() : std::string_view(track.pieces.front().url);
}

// How a stored track and its format fit together: the same kind of delivery, or not at all.
enum class Fit {
    WholeFile,
    Playlist,
    Pieces,
    None,
};

Fit FitOf(const model::Download& download, const model::MediaTrack& track, const OfferedFormat& format)
{
    if (std::holds_alternative<media::PlaylistAddress>(*format.source)) {
        return download.kind == model::DownloadKind::Hls ? Fit::Playlist : Fit::None;
    }
    if (download.kind != model::DownloadKind::Dash) {
        return Fit::None;
    }
    if (track.wholeFile) {
        return WholeFileUrl(format) ? Fit::WholeFile : Fit::None;
    }
    return Fit::Pieces;
}

}  // namespace

std::optional<std::int64_t> AddressExpiresAtUnixMs(std::string_view url)
{
    const auto parsed = ParseUrl(url);
    if (!parsed) {
        return std::nullopt;
    }
    std::string_view query = parsed->query;
    while (!query.empty()) {
        const size_t amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        if (pair.starts_with("expire=")) {
            const std::string_view value = pair.substr(7);
            std::int64_t seconds = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), seconds);
            // Seconds of a date between 2001 and 2286, as an address would give.
            if (error == std::errc{} && end == value.data() + value.size() && seconds >= 1'000'000'000 &&
                seconds < 10'000'000'000) {
                return seconds * 1000;
            }
            return std::nullopt;
        }
        if (amp == std::string_view::npos) {
            break;
        }
        query.remove_prefix(amp + 1);
    }
    return std::nullopt;
}

bool NeedsFreshAddresses(const model::Download& download, std::int64_t nowUnixMs)
{
    if (!download.siteVideo) {
        return false;
    }
    const std::int64_t margin = std::chrono::milliseconds(kSiteAddressMargin).count();
    for (const model::MediaTrack& track : download.tracks) {
        if (track.siteFormat.empty()) {
            continue;
        }
        // A file's or first piece's address, and for HLS the playlist's own.
        for (const std::string_view address : {AddressOf(track), std::string_view(track.sourceUrl)}) {
            const auto expires = AddressExpiresAtUnixMs(address);
            if (expires && *expires - margin <= nowUnixMs) {
                return true;
            }
        }
    }
    return false;
}

bool LooksLikeExpiredAddress(const TransferFailure& failure)
{
    return failure.httpStatus == 403 || failure.httpStatus == 410;
}

FreshAddresses ApplyFreshAddresses(model::Download& download, const media::SiteVideo& video)
{
    FreshAddresses result;
    // Checked first, so a missing format changes nothing at all.
    std::vector<std::optional<OfferedFormat>> formats;
    for (const model::MediaTrack& track : download.tracks) {
        if (track.siteFormat.empty() || (track.wholeFile && track.pieces.empty())) {
            formats.emplace_back();
            continue;
        }
        auto format = FindFormat(video.offer, track.siteFormat);
        if (!format) {
            result.failure = "The site no longer offers the quality this download was started with.";
            return result;
        }
        if (FitOf(download, track, *format) == Fit::None) {
            result.failure = "The site now offers the quality this download was started with in another way.";
            return result;
        }
        formats.push_back(std::move(format));
    }
    // The headers go with the first track's format, as a quality's go with its picture (QualityTracks).
    const auto first = std::find_if(formats.begin(), formats.end(), [](const auto& format) { return format.has_value(); });
    const auto firstFormat = static_cast<std::uint32_t>(first - formats.begin());
    for (std::uint32_t t = 0; t < download.tracks.size(); ++t) {
        if (!formats[t]) {
            continue;
        }
        model::MediaTrack& track = download.tracks[t];
        const OfferedFormat& format = *formats[t];
        if (t == firstFormat) {
            result.headers = format.site->headers;
        }
        switch (FitOf(download, track, format)) {
        case Fit::Playlist:
            // Its pieces' addresses may be signed too: the playlist is always read again.
            track.sourceUrl = std::get<media::PlaylistAddress>(*format.source).url;
            track.segmentQuery = format.site->segmentQuery;
            track.keyQuery = format.site->keyQuery;
            result.relist.push_back(t);
            break;
        case Fit::Pieces: {
            model::MediaTrack fresh = track;
            FillTrack(fresh, std::get<media::TrackSegments>(*format.source));
            if (!KeepProgress(fresh, track)) {
                result.restarted.push_back(t);
            }
            track.keys = std::move(fresh.keys);
            track.pieces = std::move(fresh.pieces);
            track.ranges = std::move(fresh.ranges);
            track.wholeFile = fresh.wholeFile;
            result.changed.push_back(t);
            break;
        }
        case Fit::WholeFile: {
            const std::string url = *WholeFileUrl(format);
            const std::optional<std::int64_t> bytes = format.site->bytes;
            const bool sameBytes = track.expectedBytes && bytes && *track.expectedBytes == *bytes;
            if (!sameBytes) {
                track.ranges.clear();
                track.pieces.front().done = false;
                track.pieces.front().bytes.reset();
                track.expectedBytes = bytes;
                result.restarted.push_back(t);
            } else if (AddressOf(track) == url) {
                break;  // nothing new
            }
            track.pieces.front().url = url;
            result.changed.push_back(t);
            break;
        }
        case Fit::None:
            break;  // refused above
        }
    }
    return result;
}

}  // namespace ldm::core
