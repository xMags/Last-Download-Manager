#pragma once

#include "base/Result.h"
#include "media/MediaModel.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// A video page of a supported site, as the site resolver (yt-dlp, run by platform/win) described
// it in its JSON answer. The answer is untrusted input: it is read field by field, bounded, and
// turned into the same MediaOffer a DASH manifest gives, so the quality list, the stream choice
// and the downloads treat it like any other stream. Every quality is one whole file per track.
namespace ldm::media {

// Bounds on one answer. A long YouTube video answers with about 0.2 MB and 60 formats.
inline constexpr size_t kMaxSiteAnswerBytes = 16 * 1024 * 1024;
inline constexpr size_t kMaxSiteFormats = 500;
inline constexpr size_t kMaxSiteUrlBytes = 16 * 1024;

struct SiteVideo {
    std::string id;     // the site's own id of the video
    std::string title;  // may be empty
    std::optional<double> seconds;
    // Picked formats only: one video quality per height and frame rate, the best audio per
    // language and codec. Separate sound, so the offer's format is Dash. Never live, never DRM.
    MediaOffer offer;
};

// Reads an answer. Fails, with words to show the user, for a playlist, a live stream, a video
// with nothing LDM can download (protected, or only kinds of formats it does not take), and for
// anything that is not a well-formed answer.
[[nodiscard]] base::Result<SiteVideo> ParseSiteVideo(std::string_view json);

}  // namespace ldm::media
