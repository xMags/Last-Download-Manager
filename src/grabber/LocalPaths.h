#pragma once

#include "core/Url.h"

#include <string>
#include <string_view>

// Where the Grabber saves what it finds, below a project's save folder (checklist G-02), and how
// a saved page refers to it for offline browsing.
namespace ldm::grabber {

// The '/'-separated path below the save folder: "<host>/<folders>/<name>" with `keepSiteFolders`,
// else "<name>". Every part is percent-decoded and made a safe Windows name. A page's name ends
// in ".html" (a folder's page is "index.html"); an address with a query gets "_<8 hex digits>"
// of the query before its extension, so "view.php?id=1" and "?id=2" stay apart.
[[nodiscard]] std::string LocalRelativePath(const core::Url& url, bool page, bool keepSiteFolders);

// A relative reference from the page saved at `fromPath` to the item saved at `toPath`, both
// LocalRelativePath results. Percent-encoded except for unreserved characters and '/', so it is
// safe in any attribute, quoted or not.
[[nodiscard]] std::string RelativeReference(std::string_view fromPath, std::string_view toPath);

// Percent-encodes an address for an attribute: spaces, quotes, '<', '>', '&' and ',' (srcset
// separates its candidates with commas), and bytes outside ASCII.
[[nodiscard]] std::string AttributeSafeAddress(std::string_view address);

}  // namespace ldm::grabber
