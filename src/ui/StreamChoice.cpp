#include "ui/StreamChoice.h"

#include "core/FileNames.h"
#include "core/QualityTracks.h"

namespace ldm::ui {

namespace {

base::Error Refused(std::string message)
{
    return base::MakeError(base::ErrorCode::Unsupported, std::move(message));
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

}  // namespace

base::Result<StreamCapture> BuildStreamCapture(const TabMedia& tab, const MediaItem& item, const PanelAction& action)
{
    if (!item.offer) {
        return Refused("This stream is not ready yet.");
    }
    const media::MediaOffer& offer = *item.offer;
    // A live HLS stream is recorded; a live DASH stream is not (M3 design, known limits). A site's
    // live video offers HLS only.
    if (offer.live && item.kind == browser::MediaKind::Dash && !item.siteVideo) {
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
        capture.media.tracks.push_back(core::TrackForRendition(subtitles, capture.manifestUrl, model::TrackRole::Subtitles));
        if (item.siteVideo) {
            capture.media.kind = core::SiteDownloadKind(capture.media.tracks).Value();  // one track: never mixed
        }
        const std::string language = subtitles.language.empty() ? std::string() : core::SanitizeFileName(subtitles.language);
        capture.fileName = Title(tab, item) + (language.empty() ? "" : "." + language) + ".vtt";
        return capture;
    }

    if (action.kind != PanelAction::Kind::DownloadStream || action.entry >= item.qualities.entries.size()) {
        return Refused("This quality is no longer offered.");
    }
    const media::QualityEntry& entry = item.qualities.entries[action.entry];
    auto quality = core::TracksForQuality(offer, entry, capture.manifestUrl);
    if (!quality) {
        return quality.GetError();
    }
    capture.media.tracks = std::move(quality.Value().tracks);
    if (!quality.Value().referer.empty()) {
        capture.pageUrl = quality.Value().referer;  // the Referer the site wants for its formats
    }
    if (item.siteVideo) {
        auto kind = core::SiteDownloadKind(capture.media.tracks);
        if (!kind) {
            return kind.GetError();
        }
        capture.media.kind = kind.Value();
    }
    if (!offer.live) {
        capture.media.approximateBytes = entry.approximateBytes;  // a recording's size depends on how long it runs
    }
    capture.fileName = Title(tab, item) + (quality.Value().picture ? ".mp4" : ".m4a");
    return capture;
}

}  // namespace ldm::ui
