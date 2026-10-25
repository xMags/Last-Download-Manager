#include "core/SiteList.h"

#include "core/NetworkAddress.h"
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

// A path segment a listed site uses for a video's id or name: letters, digits, '-', '_' and '.',
// not empty, not a dot segment, and short.
bool IsSlug(std::string_view text)
{
    return !text.empty() && text.size() <= 200 && text != "." && text != ".." &&
           std::all_of(text.begin(), text.end(), [](char c) {
               return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                      c == '.';
           });
}

bool IsNumber(std::string_view text)
{
    return !text.empty() && text.size() <= 20 && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// The path's segments, with one trailing slash allowed; nothing for an empty segment inside.
std::optional<std::vector<std::string_view>> Segments(std::string_view path)
{
    std::vector<std::string_view> segments;
    if (path.empty() || path.front() != '/') {
        return std::nullopt;
    }
    path.remove_prefix(1);
    if (!path.empty() && path.back() == '/') {
        path.remove_suffix(1);
    }
    while (!path.empty()) {
        const size_t slash = path.find('/');
        const std::string_view segment = path.substr(0, slash);
        if (segment.empty()) {
            return std::nullopt;
        }
        segments.push_back(segment);
        if (slash == std::string_view::npos) {
            break;
        }
        path.remove_prefix(slash + 1);
    }
    return segments;
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

bool HostIs(std::string_view host, std::initializer_list<std::string_view> names)
{
    return std::find(names.begin(), names.end(), host) != names.end();
}

std::string Join(std::string_view root, const std::vector<std::string_view>& segments)
{
    std::string address(root);
    for (const std::string_view segment : segments) {
        address += '/';
        address += segment;
    }
    return address;
}

std::optional<std::string> YouTubeVideo(const Url& url, const std::vector<std::string_view>& path)
{
    const bool main = HostIs(url.host, {"youtube.com", "www.youtube.com", "m.youtube.com"});
    const bool embed = HostIs(url.host, {"youtube-nocookie.com", "www.youtube-nocookie.com"});
    std::optional<std::string_view> id;
    if (url.host == "youtu.be" && path.size() == 1) {
        id = path[0];
    } else if (main && path.size() == 1 && path[0] == "watch" && url.path == "/watch") {
        id = QueryValue(url.query, "v");
    } else if ((main || embed) && path.size() == 2 && url.path.find("//") == std::string::npos) {
        const bool kind = embed ? path[0] == "embed" : (path[0] == "shorts" || path[0] == "embed" || path[0] == "live");
        if (kind) {
            id = path[1];
        }
    }
    if (!id || !IsYouTubeId(*id)) {
        return std::nullopt;
    }
    return "https://www.youtube.com/watch?v=" + std::string(*id);
}

std::optional<std::string> DailymotionVideo(const Url& url, const std::vector<std::string_view>& path)
{
    if (HostIs(url.host, {"dailymotion.com", "www.dailymotion.com"}) && path.size() == 2 && path[0] == "video" && IsSlug(path[1])) {
        return Join("https://www.dailymotion.com", path);
    }
    if (url.host == "dai.ly" && path.size() == 1 && IsSlug(path[0])) {
        return "https://www.dailymotion.com/video/" + std::string(path[0]);
    }
    return std::nullopt;
}

std::optional<std::string> VimeoVideo(const Url& url, const std::vector<std::string_view>& path)
{
    if (HostIs(url.host, {"vimeo.com", "www.vimeo.com"}) && path.size() == 1 && IsNumber(path[0])) {
        return Join("https://vimeo.com", path);
    }
    return std::nullopt;
}

std::optional<std::string> TwitchVideo(const Url& url, const std::vector<std::string_view>& path)
{
    const bool main = HostIs(url.host, {"twitch.tv", "www.twitch.tv", "m.twitch.tv"});
    if (main && path.size() == 2 && path[0] == "videos" && IsNumber(path[1])) {
        return Join("https://www.twitch.tv", path);
    }
    if (main && path.size() == 3 && path[1] == "clip" && IsSlug(path[0]) && IsSlug(path[2])) {
        return Join("https://www.twitch.tv", path);
    }
    if (url.host == "clips.twitch.tv" && path.size() == 1 && IsSlug(path[0])) {
        return Join("https://clips.twitch.tv", path);
    }
    return std::nullopt;
}

std::optional<std::string> SoundCloudTrack(const Url& url, const std::vector<std::string_view>& path)
{
    // "/<user>/<track>"; the site's own pages and a user's lists have names of their own.
    constexpr std::array<std::string_view, 20> kNotUsers{
        "discover", "stream", "upload", "you", "search", "charts", "pages", "settings", "messages", "notifications",
        "people", "terms-of-use", "jobs", "mobile", "imprint", "popular", "tags", "stations", "feed", "signin"};
    constexpr std::array<std::string_view, 12> kNotTracks{
        "sets", "likes", "reposts", "tracks", "albums", "followers", "following", "popular-tracks", "comments",
        "spotlight", "toptracks", "playlists"};
    if (!HostIs(url.host, {"soundcloud.com", "www.soundcloud.com", "m.soundcloud.com"}) || path.size() != 2 ||
        !IsSlug(path[0]) || !IsSlug(path[1]) ||
        std::find(kNotUsers.begin(), kNotUsers.end(), path[0]) != kNotUsers.end() ||
        std::find(kNotTracks.begin(), kNotTracks.end(), path[1]) != kNotTracks.end()) {
        return std::nullopt;
    }
    return Join("https://soundcloud.com", path);
}

std::optional<std::string> TedTalk(const Url& url, const std::vector<std::string_view>& path)
{
    if (HostIs(url.host, {"ted.com", "www.ted.com"}) && path.size() == 2 && path[0] == "talks" && IsSlug(path[1])) {
        return Join("https://www.ted.com", path);
    }
    return std::nullopt;
}

std::optional<std::string> BandcampTrack(const Url& url, const std::vector<std::string_view>& path)
{
    // "<artist>.bandcamp.com/track/<name>"
    constexpr std::string_view kSuffix = ".bandcamp.com";
    if (!url.host.ends_with(kSuffix) || url.host.size() == kSuffix.size() || path.size() != 2 || path[0] != "track" ||
        !IsSlug(path[1])) {
        return std::nullopt;
    }
    const std::string_view artist = std::string_view(url.host).substr(0, url.host.size() - kSuffix.size());
    if (!IsSlug(artist) || artist.find('.') != std::string_view::npos || artist == "www") {
        return std::nullopt;
    }
    return Join("https://" + url.host, path);
}

std::optional<std::string> InstagramReel(const Url& url, const std::vector<std::string_view>& path)
{
    if (HostIs(url.host, {"instagram.com", "www.instagram.com"}) && path.size() == 2 &&
        (path[0] == "reel" || path[0] == "reels") && IsSlug(path[1])) {
        return "https://www.instagram.com/reel/" + std::string(path[1]) + "/";
    }
    return std::nullopt;
}

using SiteMatcher = std::optional<std::string> (*)(const Url&, const std::vector<std::string_view>&);

// The listed sites: each one passed a manual test of its pages (round 2 design, section 4 step 9).
// Pages where a video is only sometimes found (a post of a forum or a social site) are left to
// the user's request, so browsing them never starts the resolver.
struct ListedSite {
    std::array<std::string_view, 4> hosts;  // for the extension; empty entries are unused
    SiteMatcher match;
};

constexpr std::array<ListedSite, 8> kListedSites{{
    {{"youtube.com", "youtu.be", "youtube-nocookie.com", ""}, YouTubeVideo},
    {{"dailymotion.com", "dai.ly", "", ""}, DailymotionVideo},
    {{"vimeo.com", "", "", ""}, VimeoVideo},
    {{"twitch.tv", "", "", ""}, TwitchVideo},
    {{"soundcloud.com", "", "", ""}, SoundCloudTrack},
    {{"ted.com", "", "", ""}, TedTalk},
    {{"bandcamp.com", "", "", ""}, BandcampTrack},
    {{"instagram.com", "", "", ""}, InstagramReel},
}};

}  // namespace

std::optional<SitePage> CheckSitePage(std::string_view pageUrl)
{
    if (const size_t hash = pageUrl.find('#'); hash != std::string_view::npos) {
        pageUrl = pageUrl.substr(0, hash);  // never part of a request
    }
    const auto first = pageUrl.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    pageUrl = pageUrl.substr(first, pageUrl.find_last_not_of(" \t\r\n") - first + 1);
    if (pageUrl.size() > kMaxSitePageBytes ||
        std::any_of(pageUrl.begin(), pageUrl.end(), [](char c) { return static_cast<unsigned char>(c) <= 0x20 || c == 0x7f; })) {
        return std::nullopt;
    }
    const auto url = ParseUrl(pageUrl);
    if (!url || (url->scheme != "https" && url->scheme != "http") || url->hasUserInfo || IsPrivateHost(url->host)) {
        return std::nullopt;
    }
    SitePage page;
    page.address = std::string(pageUrl);
    const bool standardPort = !url->port || *url->port == 443 || *url->port == 80;
    const auto path = Segments(url->path);
    if (!standardPort || !path) {
        return page;
    }
    for (const ListedSite& site : kListedSites) {
        if (auto address = site.match(*url, *path)) {
            page.address = std::move(*address);
            page.automatic = true;
            break;
        }
    }
    return page;
}

std::vector<std::string> AutomaticSiteHosts()
{
    std::vector<std::string> hosts;
    for (const ListedSite& site : kListedSites) {
        for (const std::string_view host : site.hosts) {
            if (!host.empty()) {
                hosts.emplace_back(host);
            }
        }
    }
    return hosts;
}

}  // namespace ldm::core
