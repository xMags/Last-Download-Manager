#pragma once

#include "core/Url.h"
#include "model/GrabberProject.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The Grabber's explore and file filters (checklist G-03, G-04), as pure decisions.
namespace ldm::grabber {

// The start page's site, against which the "base site" rules are judged.
struct BaseSite {
    std::string host;        // lower case
    std::string mainDomain;  // the domain the host belongs to: "example.co.uk" for "www.a.example.co.uk"
    std::string folder;      // the start page's path up to and including its last '/'
};

[[nodiscard]] BaseSite BaseSiteOf(const core::Url& startPage);

// The domain a host belongs to: its last two labels, or three when the last is a two-letter
// country code and the one before it is short ("co.uk", "com.au"). An address literal is its own
// domain. A heuristic: LDM has no public suffix list.
[[nodiscard]] std::string MainDomainOf(std::string_view host);

// The start page's host, or with `mainDomainSites` any host in its main domain.
[[nodiscard]] bool OnBaseSite(const BaseSite& site, const core::Url& url, bool mainDomainSites);

// Where a page is in the crawl. The start page is {0, 0}.
struct PageDepth {
    int depth = 0;            // links followed from the start page
    int otherSitesDepth = 0;  // links followed since the crawl left the base site; 0 on it

    bool operator==(const PageDepth&) const = default;
};

// The depth of a page linked from a page at `parent`.
[[nodiscard]] PageDepth DepthOfLink(const PageDepth& parent, bool onBaseSite);

// Whether the explorer reads the page at `url` to look for more links: within the depths, below
// the start page's folder when parent directories are excluded (base site only), and through the
// path masks.
[[nodiscard]] bool ShouldExplore(const model::GrabberExploreFilters& filters, const BaseSite& site,
                                 const core::Url& url, PageDepth depth);

enum class FileMatch {
    Match,
    Filtered,
    NeedsSize,  // passes everything but a size limit, and the size is not known yet
};

[[nodiscard]] FileMatch MatchFile(const model::GrabberFileFilters& files, const BaseSite& site, bool mainDomainSites,
                                  const core::Url& url, std::optional<std::int64_t> size);

// The address with what does not change the resource removed, for finding duplicates: lower-case
// scheme and host (ParseUrl's), no default port, no fragment.
[[nodiscard]] std::string NormalizeAddress(const core::Url& url);

// Whether an address probably names a web page rather than a file, by its extension: none, a
// folder, or one of the page extensions (htm, html, php, asp, ...). The response type decides.
[[nodiscard]] bool LooksLikePage(const core::Url& url);

// text/html and application/xhtml+xml, with any parameters.
[[nodiscard]] bool IsPageType(std::string_view contentType);

}  // namespace ldm::grabber
