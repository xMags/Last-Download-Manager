#include "ui/StreamChoice.h"

#include "core/FileNames.h"

#include <algorithm>
#include <variant>

namespace ldm::ui {

namespace {

base::Error Refused(std::string message)
{
    return base::MakeError(base::ErrorCode::Unsupported, std::move(message));
}

// A track from where its list comes from: an HLS media playlist to read at the start, or a list
// the manifest already gave (DASH, or an HLS media playlist the page loaded itself).
core::MediaTrackChoice TrackFrom(const media::SegmentSource& source, const std::string& manifestUrl,
                                 model::TrackRole role)
{
    core::MediaTrackChoice track;
    track.role = role;
    if (const auto* address = std::get_if<media::PlaylistAddress>(&source)) {
        track.sourceUrl = address->url;
    } else {
        track.sourceUrl = manifestUrl;
        track.segments = std::get<media::TrackSegments>(source);
    }
    return track;
}

core::MediaTrackChoice RenditionTrack(const media::Rendition& rendition, const std::string& manifestUrl,
                                      model::TrackRole role)
{
    core::MediaTrackChoice track = TrackFrom(*rendition.source, manifestUrl, role);
    track.codecs = rendition.codecs;
    track.language = rendition.language;
    track.name = rendition.name;
    return track;
}

// SanitizeFileName gives a fallback name for empty text, so emptiness is checked before it.
std::string Title(const std::string& title)
{
    const bool blank = title.find_first_not_of(" \t") == std::string::npos;
    return blank ? std::string("video") : core::SanitizeFileName(title);
}

// A site video is named by its own title; the page's carries the site's name too.
std::string Title(const TabMedia& tab, const MediaItem& item)
{
    return Title(item.siteVideo && !item.siteTitle.empty() ? item.siteTitle : tab.pageTitle);
}

// The site's format of a track, so a refresh can ask for the same bytes again, and the Referer
// the site wants for it, which then goes with the whole download.
void TakeSiteFormat(core::MediaTrackChoice& track, const std::optional<media::SiteFormat>& site, StreamCapture& capture)
{
    if (site) {
        track.siteFormat = site->id;
        track.expectedBytes = site->bytes;
        if (!site->referer.empty()) {
            capture.pageUrl = site->referer;
        }
    }
}

// A site video's quality is all whole files and DASH piece lists (a Dash download) or all HLS
// playlists (an Hls one); the site offer never pairs the two kinds.
base::Result<model::DownloadKind> SiteKind(const core::MediaChoice& media)
{
    const bool lists = std::all_of(media.tracks.begin(), media.tracks.end(),
                                   [](const core::MediaTrackChoice& t) { return t.segments.has_value(); });
    const bool playlists = std::none_of(media.tracks.begin(), media.tracks.end(),
                                        [](const core::MediaTrackChoice& t) { return t.segments.has_value(); });
    if (lists) {
        return model::DownloadKind::Dash;
    }
    if (playlists) {
        return model::DownloadKind::Hls;
    }
    return Refused("This quality mixes kinds of tracks LDM cannot download together.");
}

}  // namespace

base::Result<StreamCapture> BuildStreamCapture(const TabMedia& tab, const MediaItem& item, const PanelAction& action)
{
    if (!item.offer) {
        return Refused("This stream is not ready yet.");
    }
    const media::MediaOffer& offer = *item.offer;
    // A live HLS stream is recorded; a live DASH stream is not (M3 design, known limits).
    if (offer.live && item.kind == browser::MediaKind::Dash) {
        return Refused("LDM records live HLS streams only. This is a live DASH stream.");
    }
    if (item.qualities.drm || offer.drm) {
        return Refused("This video is protected and cannot be downloaded.");
    }
    StreamCapture capture;
    capture.manifestUrl = item.finalUrl.empty() ? item.url : item.finalUrl;
    capture.pageUrl = tab.pageUrl;
    capture.userAgent = tab.userAgent;
    capture.cookies = item.cookies;
    capture.media.kind = item.kind == browser::MediaKind::Dash ? model::DownloadKind::Dash : model::DownloadKind::Hls;
    capture.media.fromPrivateNetwork = item.fromPrivateNetwork;
    capture.media.siteVideo = item.siteVideo;

    if (action.kind == PanelAction::Kind::DownloadSubtitles) {
        if (action.entry >= item.qualities.subtitles.size()) {
            return Refused("These subtitles are no longer offered.");
        }
        const size_t index = item.qualities.subtitles[action.entry];
        if (index >= offer.subtitles.size() || !offer.subtitles[index].source) {
            return Refused("These subtitles are no longer offered.");
        }
        const media::Rendition& subtitles = offer.subtitles[index];
        core::MediaTrackChoice track = RenditionTrack(subtitles, capture.manifestUrl, model::TrackRole::Subtitles);
        TakeSiteFormat(track, subtitles.site, capture);
        capture.media.tracks.push_back(std::move(track));
        if (item.siteVideo) {
            capture.media.kind = SiteKind(capture.media).Value();  // one track: never mixed
        }
        const std::string language = subtitles.language.empty() ? std::string() : core::SanitizeFileName(subtitles.language);
        capture.fileName = Title(tab, item) + (language.empty() ? "" : "." + language) + ".vtt";
        return capture;
    }

    if (action.kind != PanelAction::Kind::DownloadStream || action.entry >= item.qualities.entries.size()) {
        return Refused("This quality is no longer offered.");
    }
    const media::QualityEntry& entry = item.qualities.entries[action.entry];
    bool picture = false;
    if (entry.variant) {
        if (*entry.variant >= offer.variants.size()) {
            return Refused("This quality is no longer offered.");
        }
        const media::Variant& variant = offer.variants[*entry.variant];
        // An HLS variant without a picture is a sound-only quality of the stream.
        const model::TrackRole role =
            entry.kind == media::QualityKind::AudioOnly ? model::TrackRole::Audio : model::TrackRole::Video;
        core::MediaTrackChoice track = TrackFrom(variant.source, capture.manifestUrl, role);
        track.codecs = variant.codecs;
        TakeSiteFormat(track, variant.site, capture);
        capture.media.tracks.push_back(std::move(track));
        picture = role == model::TrackRole::Video;
    }
    if (entry.audio && entry.kind != media::QualityKind::VideoOnly) {
        if (*entry.audio >= offer.audio.size()) {
            return Refused("This quality is no longer offered.");
        }
        const media::Rendition& audio = offer.audio[*entry.audio];
        if (audio.source) {  // without one, the sound is inside the variant's own segments
            core::MediaTrackChoice track = RenditionTrack(audio, capture.manifestUrl, model::TrackRole::Audio);
            TakeSiteFormat(track, audio.site, capture);
            capture.media.tracks.push_back(std::move(track));
        }
    }
    if (capture.media.tracks.empty()) {
        return Refused("This quality has nothing to download.");
    }
    if (item.siteVideo) {
        auto kind = SiteKind(capture.media);
        if (!kind) {
            return kind.GetError();
        }
        capture.media.kind = kind.Value();
    }
    if (!offer.live) {
        capture.media.approximateBytes = entry.approximateBytes;  // a recording's size depends on how long it runs
    }
    capture.fileName = Title(tab, item) + (picture ? ".mp4" : ".m4a");
    return capture;
}

}  // namespace ldm::ui
