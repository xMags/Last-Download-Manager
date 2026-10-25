#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The pages LDM asks the site resolver (yt-dlp) about (video-sites designs, round 1 section 4 and
// round 2 section 2.2). Any http(s) page may be asked about when the user asks for it; the media
// panel asks by itself only about pages that are certainly one video of a site on LDM's list, so
// ordinary browsing never starts the resolver. A listed site's video page is asked about at an
// address LDM builds itself from the video's id: nothing else a page put in its address (a
// playlist, a start time) reaches the resolver.
namespace ldm::core {

// The longest page address LDM asks about.
inline constexpr size_t kMaxSitePageBytes = 2048;

struct SitePage {
    // What the resolver is asked about: a listed video page's own address as LDM builds it, or
    // another page's address without its #fragment.
    std::string address;
    // A video page of a listed site, which the media panel looks up by itself.
    bool automatic = false;
};

// Nothing for anything but an http(s) address of a public host, without a user name or password,
// of at most kMaxSitePageBytes and without control characters or spaces.
[[nodiscard]] std::optional<SitePage> CheckSitePage(std::string_view pageUrl);

// The hosts of the listed sites, for the browser extension to report pages of (the panel's
// automatic offer). A host matches itself and its subdomains.
[[nodiscard]] std::vector<std::string> AutomaticSiteHosts();

// A site whose videos may need the user's account (video-sites round 2, decision E1): with the
// user's consent in Options, the browser hands LDM the cookies it sends to the site's pages, and
// LDM hands them to the resolver for that page only.
struct SignInSite {
    std::string_view key;   // stored in Settings::siteVideosSignIn
    std::string_view name;  // shown in Options
    std::string_view hosts;  // space-separated; each matches itself and its subdomains
    bool flagsAccounts = false;  // the site is known to flag accounts that programs use (YouTube)
};

[[nodiscard]] const std::vector<SignInSite>& SignInSites();

// The hosts of the sites `setting` (space-separated keys) turns on; unknown keys are ignored.
[[nodiscard]] std::vector<std::string> SignInHosts(std::string_view setting);

// Whether `setting` turns sign-in on for the site of `pageUrl`.
[[nodiscard]] bool UsesSignIn(std::string_view setting, std::string_view pageUrl);

}  // namespace ldm::core
