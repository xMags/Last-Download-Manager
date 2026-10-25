#pragma once

#include <optional>
#include <string>
#include <string_view>

// The video sites whose pages LDM asks the site resolver about (yt-dlp; video-sites design,
// section 4). Only pages that are certainly one video of a listed site match, so ordinary browsing
// never starts the resolver. A match gives the address LDM builds itself from the video's id:
// the page's own address never reaches the resolver, so no playlist, start time or anything a page
// put in its address can change what is asked.
namespace ldm::core {

enum class VideoSite {
    YouTube,
};

struct SiteVideoPage {
    VideoSite site = VideoSite::YouTube;
    std::string videoId;
    std::string canonicalUrl;  // "https://www.youtube.com/watch?v=<id>"
};

[[nodiscard]] std::optional<SiteVideoPage> FindSiteVideo(std::string_view pageUrl);

[[nodiscard]] std::string_view SiteName(VideoSite site) noexcept;

}  // namespace ldm::core
