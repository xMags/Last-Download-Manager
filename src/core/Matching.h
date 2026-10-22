#pragma once

#include "core/Url.h"
#include "model/Category.h"
#include "model/ConnectionRules.h"

#include <optional>
#include <span>
#include <string_view>

namespace ldm::core {

// Case-insensitive (ASCII) match where '*' stands for any sequence, including an empty one.
[[nodiscard]] bool WildcardMatch(std::string_view pattern, std::string_view text);

// Category for a new download (checklist W-02, W-03): a category whose site list matches the host
// wins; otherwise one whose extension list contains the file's extension. Nothing when neither
// matches; the caller then uses General.
[[nodiscard]] std::optional<model::CategoryId> MatchCategory(std::span<const model::Category> categories,
                                                             std::string_view fileName, const Url& url);

// Per-server connection cap (checklist ENG-04). The most specific matching pattern wins: an exact
// host before wildcards, then the longest pattern.
[[nodiscard]] std::optional<int> MatchServerLimit(std::span<const model::ServerLimit> limits, std::string_view host);

// Stored credentials for an address (checklist O-07). A pattern is "host" or "host/path-prefix" with
// '*' wildcards in the host; the longest matching pattern wins.
[[nodiscard]] const model::SiteLogin* MatchSiteLogin(std::span<const model::SiteLogin> logins, const Url& url);

}  // namespace ldm::core
