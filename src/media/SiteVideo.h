#pragma once

#include "base/Result.h"
#include "media/MediaModel.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// A video page of a site the resolver (yt-dlp, run by platform/win) reads, as it described it in
// its JSON answer. The answer is untrusted input: it is read field by field, bounded, and turned
// into the same MediaOffer a manifest gives, so the quality list, the stream choice and the
// downloads treat it like any other stream. A format is one whole file, an HLS media playlist, or
// a list of DASH pieces; every one is downloaded by LDM's own engine.
namespace ldm::media {

// Bounds on one answer. A long YouTube video answers with about 0.2 MB and 60 formats.
inline constexpr size_t kMaxSiteAnswerBytes = 16 * 1024 * 1024;
inline constexpr size_t kMaxSiteFormats = 500;
inline constexpr size_t kMaxSiteUrlBytes = 16 * 1024;
inline constexpr size_t kMaxSiteCookies = 50;

// A cookie the resolver got from the site itself while reading the page (not one of the
// browser's), which the site wants back with the requests for its files.
struct SiteCookie {
    std::string domain;     // without a leading dot
    bool hostOnly = true;   // false: also sent to subdomains of `domain`
    std::string path = "/";
    bool secure = false;
    std::int64_t expires = 0;  // Unix seconds; 0 for a session cookie
    std::string name;
    std::string value;
};

struct SiteVideo {
    std::string id;         // the site's own id of the video
    std::string title;      // may be empty
    std::string extractor;  // the resolver's name for the site, such as "Youtube"; may be empty
    std::optional<double> seconds;
    // Picked formats only: one video quality per height and frame rate, the best audio per kind
    // of delivery, language and codec. A picture whose sound is separate names its delivery as
    // its audioGroup, and the audio of that delivery carries it as groupId, so a quality pairs
    // only tracks LDM downloads the same way. Never DRM; a live stream (`offer.live`) offers its
    // HLS formats only.
    MediaOffer offer;
    std::vector<SiteCookie> cookies;
};

// Reads an answer. Fails, with words to show the user, for a playlist, a stream that has not
// started yet, a video with nothing LDM can download (protected, or only kinds of formats it does not take), and for
// anything that is not a well-formed answer.
[[nodiscard]] base::Result<SiteVideo> ParseSiteVideo(std::string_view json);

// The most videos LDM lists of one page.
inline constexpr size_t kMaxSitePlaylistEntries = 5000;

// One video of a playlist, a channel or a page with several videos.
struct SitePlaylistEntry {
    std::string url;    // its page, to ask the resolver about; may be empty for a video given in full
    std::string title;  // may be empty
    std::optional<double> seconds;
    std::string group;  // the list inside the page it is in (a channel's "Videos", "Shorts"); may be empty
    // The video in full, when the page gave it so (the videos of an ordinary page): there is no
    // page of its own to ask about.
    std::optional<SiteVideo> video;
};

struct SitePlaylist {
    std::string id;
    std::string title;
    std::vector<SitePlaylistEntry> entries;  // in the page's order; at least one
    bool truncated = false;                  // the page listed more than kMaxSitePlaylistEntries
};

using SiteAnswer = std::variant<SiteVideo, SitePlaylist>;

// Reads an answer that is one video or a list of them (a playlist, a channel and its tabs, a page
// with several videos), as yt-dlp lists them without asking about each video. Fails as
// ParseSiteVideo does, and for a list without one video LDM can download.
[[nodiscard]] base::Result<SiteAnswer> ParseSiteAnswer(std::string_view json);

}  // namespace ldm::media
