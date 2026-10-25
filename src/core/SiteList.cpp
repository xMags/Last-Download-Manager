#include "core/SiteList.h"

#include "core/Url.h"

#include <algorithm>
#include <array>

namespace ldm::core {

namespace {

// YouTube video ids are 11 characters of the URL-safe Base64 alphabet.
bool IsYouTubeId(std::string_view id)
{
    return id.size() == 11 && std::all_of(id.begin(), id.end(), [](char c) {
               return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
           });
}

// The first path segment after `prefix`: "/shorts/<id>" or "/shorts/<id>/".
std::optional<std::string_view> SegmentAfter(std::string_view path, std::string_view prefix)
{
    if (!path.starts_with(prefix)) {
        return std::nullopt;
    }
    std::string_view rest = path.substr(prefix.size());
    if (const size_t slash = rest.find('/'); slash != std::string_view::npos) {
        if (slash + 1 != rest.size()) {
            return std::nullopt;  // something deeper than the video
        }
        rest = rest.substr(0, slash);
    }
    return rest;
}

// The value of `name` in a query string, as it stands (ids need no decoding).
std::optional<std::string_view> QueryValue(std::string_view query, std::string_view name)
{
    while (!query.empty()) {
        const size_t amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        if (pair.size() > name.size() && pair.starts_with(name) && pair[name.size()] == '=') {
            return pair.substr(name.size() + 1);
        }
        if (amp == std::string_view::npos) {
            break;
        }
        query.remove_prefix(amp + 1);
    }
    return std::nullopt;
}

std::optional<std::string_view> YouTubeId(const Url& url)
{
    constexpr std::array<std::string_view, 3> kMainHosts{"youtube.com", "www.youtube.com", "m.youtube.com"};
    constexpr std::array<std::string_view, 2> kEmbedHosts{"youtube-nocookie.com", "www.youtube-nocookie.com"};
    const bool main = std::find(kMainHosts.begin(), kMainHosts.end(), url.host) != kMainHosts.end();
    const bool embed = std::find(kEmbedHosts.begin(), kEmbedHosts.end(), url.host) != kEmbedHosts.end();
    if (url.host == "youtu.be") {
        return SegmentAfter(url.path, "/");
    }
    if (main && url.path == "/watch") {
        return QueryValue(url.query, "v");
    }
    if (main || embed) {
        for (const std::string_view prefix : {"/shorts/", "/embed/", "/live/"}) {
            if (embed && prefix != "/embed/") {
                continue;
            }
            if (const auto id = SegmentAfter(url.path, prefix)) {
                return id;
            }
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<SiteVideoPage> FindSiteVideo(std::string_view pageUrl)
{
    const auto url = ParseUrl(pageUrl);
    if (!url || (url->scheme != "https" && url->scheme != "http") || url->hasUserInfo) {
        return std::nullopt;
    }
    if (url->port && *url->port != 443 && *url->port != 80) {
        return std::nullopt;
    }
    const auto id = YouTubeId(*url);
    if (!id || !IsYouTubeId(*id)) {
        return std::nullopt;
    }
    SiteVideoPage page;
    page.site = VideoSite::YouTube;
    page.videoId = std::string(*id);
    page.canonicalUrl = "https://www.youtube.com/watch?v=" + page.videoId;
    return page;
}

std::string_view SiteName(VideoSite site) noexcept
{
    switch (site) {
    case VideoSite::YouTube:
        return "YouTube";
    }
    return "video site";
}

}  // namespace ldm::core
