#include "ui/StreamChoice.h"

#include "core/FileNames.h"

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

std::string Title(const TabMedia& tab)
{
    std::string title = core::SanitizeFileName(tab.pageTitle);
    return title.empty() ? std::string("video") : title;
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

    if (action.kind == PanelAction::Kind::DownloadSubtitles) {
        if (action.entry >= item.qualities.subtitles.size()) {
            return Refused("These subtitles are no longer offered.");
        }
        const size_t index = item.qualities.subtitles[action.entry];
        if (index >= offer.subtitles.size() || !offer.subtitles[index].source) {
            return Refused("These subtitles are no longer offered.");
        }
        const media::Rendition& subtitles = offer.subtitles[index];
        capture.media.tracks.push_back(RenditionTrack(subtitles, capture.manifestUrl, model::TrackRole::Subtitles));
        const std::string language = core::SanitizeFileName(subtitles.language);
        capture.fileName = Title(tab) + (language.empty() ? "" : "." + language) + ".vtt";
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
        capture.media.tracks.push_back(std::move(track));
        picture = role == model::TrackRole::Video;
    }
    if (entry.audio && entry.kind != media::QualityKind::VideoOnly) {
        if (*entry.audio >= offer.audio.size()) {
            return Refused("This quality is no longer offered.");
        }
        const media::Rendition& audio = offer.audio[*entry.audio];
        if (audio.source) {  // without one, the sound is inside the variant's own segments
            capture.media.tracks.push_back(RenditionTrack(audio, capture.manifestUrl, model::TrackRole::Audio));
        }
    }
    if (capture.media.tracks.empty()) {
        return Refused("This quality has nothing to download.");
    }
    if (!offer.live) {
        capture.media.approximateBytes = entry.approximateBytes;  // a recording's size depends on how long it runs
    }
    capture.fileName = Title(tab) + (picture ? ".mp4" : ".m4a");
    return capture;
}

}  // namespace ldm::ui
