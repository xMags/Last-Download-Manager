#include "core/QualityTracks.h"

#include <algorithm>
#include <variant>

namespace ldm::core {

namespace {

base::Error Refused(std::string message)
{
    return base::MakeError(base::ErrorCode::Unsupported, std::move(message));
}

// A track from where its list comes from: an HLS media playlist to read at the start, or a list
// the manifest already gave (DASH, a site's file, or an HLS media playlist the page loaded itself).
MediaTrackChoice TrackFrom(const media::SegmentSource& source, const std::string& manifestUrl, model::TrackRole role)
{
    MediaTrackChoice track;
    track.role = role;
    if (const auto* address = std::get_if<media::PlaylistAddress>(&source)) {
        track.sourceUrl = address->url;
    } else {
        track.sourceUrl = manifestUrl;
        track.segments = std::get<media::TrackSegments>(source);
    }
    return track;
}

// The site's format of a track, so a refresh can ask for the same bytes again.
void TakeSiteFormat(MediaTrackChoice& track, const std::optional<media::SiteFormat>& site, QualityTracks& result)
{
    if (site) {
        track.siteFormat = site->id;
        track.expectedBytes = site->bytes;
        track.segmentQuery = site->segmentQuery;
        track.keyQuery = site->keyQuery;
        if (!site->referer.empty()) {
            result.referer = site->referer;
        }
        result.headers = site->headers;
    }
}

std::optional<int> HeightLimit(model::SiteQuality quality)
{
    switch (quality) {
    case model::SiteQuality::UpTo2160:
        return 2160;
    case model::SiteQuality::UpTo1440:
        return 1440;
    case model::SiteQuality::UpTo1080:
        return 1080;
    case model::SiteQuality::UpTo720:
        return 720;
    case model::SiteQuality::UpTo480:
        return 480;
    default:
        return std::nullopt;
    }
}

}  // namespace

MediaTrackChoice TrackForRendition(const media::Rendition& rendition, const std::string& manifestUrl, model::TrackRole role)
{
    MediaTrackChoice track = TrackFrom(*rendition.source, manifestUrl, role);
    track.codecs = rendition.codecs;
    track.language = rendition.language;
    track.name = rendition.name;
    if (rendition.site) {
        track.siteFormat = rendition.site->id;
        track.expectedBytes = rendition.site->bytes;
        track.segmentQuery = rendition.site->segmentQuery;
        track.keyQuery = rendition.site->keyQuery;
    }
    return track;
}

base::Result<QualityTracks> TracksForQuality(const media::MediaOffer& offer, const media::QualityEntry& entry,
                                             const std::string& manifestUrl)
{
    QualityTracks result;
    if (entry.variant) {
        if (*entry.variant >= offer.variants.size()) {
            return Refused("This quality is no longer offered.");
        }
        const media::Variant& variant = offer.variants[*entry.variant];
        // An HLS variant without a picture is a sound-only quality of the stream.
        const model::TrackRole role =
            entry.kind == media::QualityKind::AudioOnly ? model::TrackRole::Audio : model::TrackRole::Video;
        MediaTrackChoice track = TrackFrom(variant.source, manifestUrl, role);
        track.codecs = variant.codecs;
        TakeSiteFormat(track, variant.site, result);
        result.tracks.push_back(std::move(track));
        result.picture = role == model::TrackRole::Video;
    }
    if (entry.audio && entry.kind != media::QualityKind::VideoOnly) {
        if (*entry.audio >= offer.audio.size()) {
            return Refused("This quality is no longer offered.");
        }
        const media::Rendition& audio = offer.audio[*entry.audio];
        if (audio.source) {  // without one, the sound is inside the variant's own segments
            MediaTrackChoice track = TrackForRendition(audio, manifestUrl, model::TrackRole::Audio);
            if (audio.site && !audio.site->referer.empty() && result.referer.empty()) {
                result.referer = audio.site->referer;
            }
            if (audio.site && result.headers.empty()) {
                result.headers = audio.site->headers;
            }
            result.tracks.push_back(std::move(track));
        }
    }
    if (result.tracks.empty()) {
        return Refused("This quality has nothing to download.");
    }
    return result;
}

base::Result<model::DownloadKind> SiteDownloadKind(const std::vector<MediaTrackChoice>& tracks)
{
    const auto listed = [](const MediaTrackChoice& t) { return t.segments.has_value(); };
    if (std::all_of(tracks.begin(), tracks.end(), listed)) {
        return model::DownloadKind::Dash;
    }
    if (std::none_of(tracks.begin(), tracks.end(), listed)) {
        return model::DownloadKind::Hls;
    }
    return Refused("This quality mixes kinds of tracks LDM cannot download together.");
}

std::optional<size_t> PickSiteQuality(const media::QualityList& list, model::SiteQuality quality)
{
    const auto& entries = list.entries;
    const auto first = [&entries](media::QualityKind kind) -> std::optional<size_t> {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].kind == kind) {
                return i;
            }
        }
        return std::nullopt;
    };
    if (quality == model::SiteQuality::AudioOnly) {
        return first(media::QualityKind::AudioOnly);
    }
    const auto limit = HeightLimit(quality);
    if (!limit) {
        const auto video = first(media::QualityKind::Video);
        return video ? video : first(media::QualityKind::AudioOnly);  // a sound-only page's best
    }
    // Pictures come highest first: the first one within the limit is the best of them.
    std::optional<size_t> lowest;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].kind != media::QualityKind::Video) {
            continue;
        }
        if (entries[i].height.value_or(0) <= *limit) {
            return i;
        }
        lowest = i;
    }
    return lowest ? lowest : first(media::QualityKind::AudioOnly);
}

}  // namespace ldm::core
