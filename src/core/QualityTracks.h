#pragma once

#include "base/Result.h"
#include "core/Commands.h"
#include "media/MediaModel.h"
#include "media/QualityList.h"
#include "model/Media.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

// The tracks a quality of a stream is made of (M3 design section 6; video-sites designs): what
// the media panel's choice and a site video added from a list both download. Pure.
namespace ldm::core {

struct QualityTracks {
    std::vector<MediaTrackChoice> tracks;
    bool picture = false;  // a video track is among them, so the file is a video
    std::string referer;   // the Referer a site wants for these formats; empty for none
    std::vector<std::string> headers;  // the site's other headers for them (media::SiteFormat::headers)
};

// A rendition's track: from its playlist address, or from the list the manifest gave, which then
// has `manifestUrl` as its source; with the site's format of it.
[[nodiscard]] MediaTrackChoice TrackForRendition(const media::Rendition& rendition, const std::string& manifestUrl,
                                                 model::TrackRole role);

// The tracks of `entry` of `offer`'s quality list: its picture, or its sound, and the sound
// paired with a picture whose sound is separate. Fails, with words to show, for an entry that does
// not fit the offer.
[[nodiscard]] base::Result<QualityTracks> TracksForQuality(const media::MediaOffer& offer, const media::QualityEntry& entry,
                                                           const std::string& manifestUrl);

// A site video's tracks are all lists, whole files among them (a Dash download), or all HLS
// playlists (an Hls one); the site offer never pairs the two.
[[nodiscard]] base::Result<model::DownloadKind> SiteDownloadKind(const std::vector<MediaTrackChoice>& tracks);

// The entry of a site video's quality list that `quality` picks: the best picture (the list
// gives it first), the best no higher than a height (or the lowest, when every picture is
// higher), or the best sound alone. Nothing when the list has no such entry.
[[nodiscard]] std::optional<size_t> PickSiteQuality(const media::QualityList& list, model::SiteQuality quality);

}  // namespace ldm::core
