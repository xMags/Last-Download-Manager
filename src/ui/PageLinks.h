#pragma once

#include "browser/Protocol.h"
#include "model/Cookie.h"

#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The list behind the Download All Links dialog (checklist B-05): one row per link the extension
// sent, what the extension found out about each, and the filters and renaming the dialog offers.
// No Win32 here, so the rules can be tested on their own.
namespace ldm::ui {

enum class LinkCheck {
    Pending,  // the extension has not answered for this link yet
    Done,
    Failed,
};

struct LinkRow {
    browser::PageLink link;
    std::string fileName;  // as shown: the server's name once known, else the page's or the address's
    std::optional<std::string> chosenName;  // sent to LDM: renamed in the dialog, or the download attribute
    std::optional<std::string> description;  // typed in the dialog; otherwise the link text may serve
    std::optional<std::int64_t> size;
    std::optional<std::string> mimeType;
    LinkCheck check = LinkCheck::Pending;
    bool checked = false;
    bool inDownloadList = false;  // the address is already in LDM's list
};

struct LinkFilter {
    bool hideHtml = true;
    bool hideImages = false;    // pictures shown on the page, not links to pictures
    bool hideRepeated = true;   // an address listed before (ignoring #fragments) or already in LDM
};

// One row per link, in the order the extension sent them, so LinkDetails indexes stay valid.
// `listed` holds the addresses already in LDM's download list. Selected links start checked;
// "all links" start unchecked, as there are usually many the user does not want.
[[nodiscard]] std::vector<LinkRow> MakeLinkRows(const browser::Links& links, const std::set<std::string>& listed);

// Applies what the extension found; ignores an index outside the rows. Returns false then.
bool ApplyLinkDetails(std::vector<LinkRow>& rows, const browser::LinkDetails& details);

// A web page rather than a file: by its type once known, by its address until then.
[[nodiscard]] bool IsHtmlLink(const LinkRow& row);

// The rows the filter lets through, as indexes into `rows`.
[[nodiscard]] std::vector<size_t> VisibleLinkRows(std::span<const LinkRow> rows, const LinkFilter& filter);

// The Type column: the extension in capitals, "HTML" for a page without one.
[[nodiscard]] std::string LinkTypeText(const LinkRow& row);

// Renames with a pattern where '*' becomes a running number: "photo*" makes photo1.jpg, photo2.png
// and so on, keeping each file's extension unless the pattern names one after the '*' ("photo*.jpg").
// Numbers are padded to the
// width of `count`. Nothing for a pattern without exactly one '*'.
[[nodiscard]] std::optional<std::string> NameFromPattern(std::string_view pattern, size_t number, size_t count,
                                                         std::string_view originalName);

// The cookies a browser would send to `url`; nothing for an address that does not parse.
[[nodiscard]] std::vector<model::Cookie> CookiesForAddress(std::span<const model::Cookie> cookies, const std::string& url);

}  // namespace ldm::ui
