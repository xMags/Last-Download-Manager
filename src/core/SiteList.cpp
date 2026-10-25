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
    if (HostIs(url.host, {"dailymotion.com", "www.dailymotion.com"}) && path.size() == 3 && path[0] == "embed" &&
        path[1] == "video" && IsSlug(path[2])) {
        return "https://www.dailymotion.com/video/" + std::string(path[2]);
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

std::optional<std::string> TedTalk(const Url& url, const std::vector<std::string_view>& path)
{
    if (HostIs(url.host, {"ted.com", "www.ted.com"}) && path.size() == 2 && path[0] == "talks" && IsSlug(path[1])) {
        return Join("https://www.ted.com", path);
    }
    return std::nullopt;
}

std::optional<std::string> InstagramReel(const Url& url, const std::vector<std::string_view>& path)
{
    if (HostIs(url.host, {"instagram.com", "www.instagram.com"}) && path.size() == 2 &&
        (path[0] == "reel" || path[0] == "reels") && IsSlug(path[1])) {
        return "https://www.instagram.com/reel/" + std::string(path[1]) + "/";
    }
    return std::nullopt;
}

// Letters and digits only, of a length within [least, most].
bool IsAlphanumeric(std::string_view text, size_t least, size_t most)
{
    return text.size() >= least && text.size() <= most && std::all_of(text.begin(), text.end(), [](char c) {
               return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
           });
}

std::optional<std::string> FacebookVideo(const Url& url, const std::vector<std::string_view>& path)
{
    if (url.host == "fb.watch" && path.size() == 1 && IsSlug(path[0])) {
        return "https://fb.watch/" + std::string(path[0]) + "/";
    }
    if (!HostIs(url.host, {"facebook.com", "www.facebook.com", "m.facebook.com", "web.facebook.com"})) {
        return std::nullopt;
    }
    if (path.size() == 2 && path[0] == "reel" && IsNumber(path[1])) {
        return "https://www.facebook.com/reel/" + std::string(path[1]);
    }
    std::optional<std::string_view> id;
    if (path.size() == 1 && (path[0] == "watch" || path[0] == "video.php")) {
        id = QueryValue(url.query, "v");
    } else if ((path.size() == 3 || path.size() == 4) && path[1] == "videos" && IsSlug(path[0]) && IsSlug(path[2])) {
        id = path.back();  // a page's video: /name/videos/id, or /name/videos/title/id
    }
    if (!id || !IsNumber(*id)) {
        return std::nullopt;
    }
    return "https://www.facebook.com/watch/?v=" + std::string(*id);
}

std::optional<std::string> RumbleVideo(const Url& url, const std::vector<std::string_view>& path)
{
    if (!HostIs(url.host, {"rumble.com", "www.rumble.com"})) {
        return std::nullopt;
    }
    // A video's page is "v" and its id, then its title: /v6abc12-the-title.html.
    if (path.size() == 1 && path[0].starts_with('v') && path[0].ends_with(".html") && IsSlug(path[0]) &&
        path[0].find('-') != std::string_view::npos) {
        return Join("https://rumble.com", path);
    }
    if (path.size() == 2 && path[0] == "embed" && path[1].starts_with('v') && IsSlug(path[1])) {
        return Join("https://rumble.com", path);
    }
    return std::nullopt;
}

// A channel's past broadcasts and its clips; not the channel's own page, which is live only at times.
std::optional<std::string> KickVideo(const Url& url, const std::vector<std::string_view>& path)
{
    if (!HostIs(url.host, {"kick.com", "www.kick.com"})) {
        return std::nullopt;
    }
    if (path.size() == 3 && path[1] == "videos" && IsSlug(path[0]) && IsSlug(path[2])) {
        return Join("https://kick.com", path);
    }
    if (path.size() == 3 && path[1] == "clips" && IsSlug(path[0]) && path[2].starts_with("clip_") && IsSlug(path[2])) {
        return Join("https://kick.com", path);
    }
    if (path.size() == 1 && IsSlug(path[0])) {
        if (const auto clip = QueryValue(url.query, "clip"); clip && clip->starts_with("clip_") && IsSlug(*clip)) {
            return "https://kick.com/" + std::string(path[0]) + "/clips/" + std::string(*clip);
        }
    }
    return std::nullopt;
}

// "video" or "clip", then the owner's number (negative for a community), '_' and the video's.
std::optional<std::string> VkVideo(const Url& url, const std::vector<std::string_view>& path)
{
    const bool vk = HostIs(url.host, {"vk.com", "www.vk.com", "m.vk.com"});
    const bool vkVideo = HostIs(url.host, {"vkvideo.ru", "www.vkvideo.ru", "m.vkvideo.ru"});
    if ((!vk && !vkVideo) || path.size() != 1) {
        return std::nullopt;
    }
    for (const std::string_view kind : {"video", "clip"}) {
        if (!path[0].starts_with(kind)) {
            continue;
        }
        std::string_view ids = path[0].substr(kind.size());
        if (ids.starts_with('-')) {
            ids.remove_prefix(1);
        }
        const size_t underscore = ids.find('_');
        if (underscore == std::string_view::npos || !IsNumber(ids.substr(0, underscore)) ||
            !IsNumber(ids.substr(underscore + 1))) {
            return std::nullopt;
        }
        return std::string(vk ? "https://vk.com/" : "https://vkvideo.ru/") + std::string(path[0]);
    }
    return std::nullopt;
}

// An LBRY claim as Odysee writes it: a name and, after ':', its id ("First-day:1", "@channel:7").
bool IsClaim(std::string_view text)
{
    const size_t colon = text.find(':');
    return colon != std::string_view::npos && colon > 0 && colon + 1 < text.size() && text.size() <= 300 &&
           std::all_of(text.begin(), text.end(), [](char c) {
               return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                      c == '_' || c == '.' || c == ':' || c == '%' || c == '@';
           });
}

std::optional<std::string> OdyseeVideo(const Url& url, const std::vector<std::string_view>& path)
{
    if (!HostIs(url.host, {"odysee.com", "www.odysee.com"})) {
        return std::nullopt;
    }
    const bool inChannel = path.size() == 2 && path[0].starts_with('@') && IsClaim(path[0]);
    if ((inChannel || path.size() == 1) && !path.back().starts_with('@') && IsClaim(path.back())) {
        return Join("https://odysee.com", path);
    }
    return std::nullopt;
}

std::optional<std::string> BitChuteVideo(const Url& url, const std::vector<std::string_view>& path)
{
    if (HostIs(url.host, {"bitchute.com", "www.bitchute.com", "old.bitchute.com"}) && path.size() == 2 &&
        (path[0] == "video" || path[0] == "embed") && IsSlug(path[1])) {
        return "https://www.bitchute.com/video/" + std::string(path[1]) + "/";
    }
    return std::nullopt;
}

// Two letters for the kind of video ("sm", "so", "nm"), or none, then a number.
bool IsNiconicoId(std::string_view id)
{
    const bool letters = id.size() > 2 && id[0] >= 'a' && id[0] <= 'z' && id[1] >= 'a' && id[1] <= 'z';
    return IsNumber(id.substr(letters ? 2 : 0));
}

std::optional<std::string> NiconicoVideo(const Url& url, const std::vector<std::string_view>& path)
{
    std::optional<std::string_view> id;
    if (HostIs(url.host, {"nicovideo.jp", "www.nicovideo.jp", "sp.nicovideo.jp"}) && path.size() == 2 &&
        path[0] == "watch") {
        id = path[1];
    } else if (url.host == "nico.ms" && path.size() == 1) {
        id = path[0];
    }
    if (!id || !IsNiconicoId(*id)) {
        return std::nullopt;
    }
    return "https://www.nicovideo.jp/watch/" + std::string(*id);
}

std::optional<std::string> OkVideo(const Url& url, const std::vector<std::string_view>& path)
{
    if (HostIs(url.host, {"ok.ru", "www.ok.ru", "m.ok.ru"}) && path.size() == 2 &&
        (path[0] == "video" || path[0] == "videoembed") && IsNumber(path[1])) {
        return "https://ok.ru/video/" + std::string(path[1]);
    }
    return std::nullopt;
}

std::optional<std::string> CoubVideo(const Url& url, const std::vector<std::string_view>& path)
{
    if (HostIs(url.host, {"coub.com", "www.coub.com"}) && path.size() == 2 && (path[0] == "view" || path[0] == "embed") &&
        IsAlphanumeric(path[1], 1, 20)) {
        return "https://coub.com/view/" + std::string(path[1]);
    }
    return std::nullopt;
}

// A video's 32 hexadecimal digits.
bool IsLoomId(std::string_view id)
{
    return id.size() == 32 &&
           std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::optional<std::string> LoomVideo(const Url& url, const std::vector<std::string_view>& path)
{
    if (HostIs(url.host, {"loom.com", "www.loom.com"}) && path.size() == 2 && (path[0] == "share" || path[0] == "embed") &&
        IsLoomId(path[1])) {
        return "https://www.loom.com/share/" + std::string(path[1]);
    }
    return std::nullopt;
}

using SiteMatcher = std::optional<std::string> (*)(const Url&, const std::vector<std::string_view>&);

// The listed sites: each one passed a manual test of its pages (round 2 design, section 4 step 9).
// Pages where a video is only sometimes found (a post of a forum or a social site) are left to
// the user's request, so browsing them never starts the resolver; so are sound-only sites
// (SoundCloud, Bandcamp), whose pages have no player for the panel to sit on.
struct ListedSite {
    std::array<std::string_view, 4> hosts;  // for the extension; empty entries are unused
    SiteMatcher match;
};

constexpr std::array<ListedSite, 16> kListedSites{{
    {{"youtube.com", "youtu.be", "youtube-nocookie.com", ""}, YouTubeVideo},
    {{"dailymotion.com", "dai.ly", "", ""}, DailymotionVideo},
    {{"vimeo.com", "", "", ""}, VimeoVideo},
    {{"twitch.tv", "", "", ""}, TwitchVideo},
    {{"ted.com", "", "", ""}, TedTalk},
    {{"instagram.com", "", "", ""}, InstagramReel},
    {{"facebook.com", "fb.watch", "", ""}, FacebookVideo},
    {{"rumble.com", "", "", ""}, RumbleVideo},
    {{"kick.com", "", "", ""}, KickVideo},
    {{"vk.com", "vkvideo.ru", "", ""}, VkVideo},
    {{"odysee.com", "", "", ""}, OdyseeVideo},
    {{"bitchute.com", "", "", ""}, BitChuteVideo},
    {{"nicovideo.jp", "nico.ms", "", ""}, NiconicoVideo},
    {{"ok.ru", "", "", ""}, OkVideo},
    {{"coub.com", "", "", ""}, CoubVideo},
    {{"loom.com", "", "", ""}, LoomVideo},
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

const std::vector<SignInSite>& SignInSites()
{
    static const std::vector<SignInSite> sites{
        {"youtube", "YouTube", "youtube.com youtu.be", true},
        {"vimeo", "Vimeo", "vimeo.com", false},
        {"instagram", "Instagram", "instagram.com", false},
        {"facebook", "Facebook", "facebook.com fb.watch", false},
        {"x", "X (Twitter)", "x.com twitter.com", false},
        {"reddit", "Reddit", "reddit.com", false},
        {"tiktok", "TikTok", "tiktok.com", false},
        {"twitch", "Twitch", "twitch.tv", false},
        {"dailymotion", "Dailymotion", "dailymotion.com", false},
        {"bilibili", "Bilibili", "bilibili.com bilibili.tv", false},
        {"patreon", "Patreon", "patreon.com", false},
        {"niconico", "Niconico", "nicovideo.jp", false},
    };
    return sites;
}

std::vector<std::string> SignInHosts(std::string_view setting)
{
    const auto words = [](std::string_view text) {
        std::vector<std::string_view> out;
        while (!text.empty()) {
            const size_t start = text.find_first_not_of(' ');
            if (start == std::string_view::npos) {
                break;
            }
            text.remove_prefix(start);
            const size_t end = std::min(text.find(' '), text.size());
            out.push_back(text.substr(0, end));
            text.remove_prefix(end);
        }
        return out;
    };
    std::vector<std::string> hosts;
    const auto keys = words(setting);
    for (const SignInSite& site : SignInSites()) {
        if (std::find(keys.begin(), keys.end(), site.key) == keys.end()) {
            continue;
        }
        for (const std::string_view host : words(site.hosts)) {
            hosts.emplace_back(host);
        }
    }
    return hosts;
}

bool UsesSignIn(std::string_view setting, std::string_view pageUrl)
{
    const auto url = ParseUrl(pageUrl);
    if (!url) {
        return false;
    }
    for (const std::string& host : SignInHosts(setting)) {
        if (url->host == host || (url->host.size() > host.size() && url->host.ends_with(host) &&
                                  url->host[url->host.size() - host.size() - 1] == '.')) {
            return true;
        }
    }
    return false;
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
