#include "core/SiteRefresh.h"

#include "core/Url.h"

#include <charconv>
#include <variant>

namespace ldm::core {

namespace {

// The format `id` of the video's offer: its address and the length the site states.
struct OfferedFormat {
    std::string url;
    std::optional<std::int64_t> bytes;
};

std::optional<OfferedFormat> FindFormat(const media::MediaOffer& offer, const std::string& id)
{
    const auto from = [](const std::optional<media::SiteFormat>& site, const media::SegmentSource& source)
        -> std::optional<OfferedFormat> {
        const auto* track = std::get_if<media::TrackSegments>(&source);
        if (!site || track == nullptr || track->segments.size() != 1) {
            return std::nullopt;
        }
        return OfferedFormat{track->segments.front().url, site->bytes};
    };
    for (const media::Variant& variant : offer.variants) {
        if (variant.site && variant.site->id == id) {
            return from(variant.site, variant.source);
        }
    }
    for (const media::Rendition& audio : offer.audio) {
        if (audio.site && audio.site->id == id && audio.source) {
            return from(audio.site, *audio.source);
        }
    }
    return std::nullopt;
}

// The track's current address: a whole-file track's only piece. Its sourceUrl is the video's page.
std::string_view AddressOf(const model::MediaTrack& track)
{
    return track.pieces.empty() ? std::string_view() : std::string_view(track.pieces.front().url);
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
        if (track.siteFormat.empty() || !track.wholeFile) {
            continue;
        }
        const auto expires = AddressExpiresAtUnixMs(AddressOf(track));
        if (expires && *expires - margin <= nowUnixMs) {
            return true;
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
        if (track.siteFormat.empty() || !track.wholeFile || track.pieces.empty()) {
            formats.emplace_back();
            continue;
        }
        auto format = FindFormat(video.offer, track.siteFormat);
        if (!format) {
            result.failure = "The site no longer offers the quality this download was started with.";
            return result;
        }
        formats.push_back(std::move(format));
    }
    for (std::uint32_t t = 0; t < download.tracks.size(); ++t) {
        if (!formats[t]) {
            continue;
        }
        model::MediaTrack& track = download.tracks[t];
        const OfferedFormat& format = *formats[t];
        const bool sameBytes = track.expectedBytes && format.bytes && *track.expectedBytes == *format.bytes;
        if (!sameBytes) {
            track.ranges.clear();
            track.pieces.front().done = false;
            track.pieces.front().bytes.reset();
            track.expectedBytes = format.bytes;
            result.restarted.push_back(t);
        } else if (AddressOf(track) == format.url) {
            continue;  // nothing new
        }
        track.pieces.front().url = format.url;
        result.changed.push_back(t);
    }
    return result;
}

}  // namespace ldm::core
