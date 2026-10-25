#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The links of a web page, read without running it (M4 design section 6.2): the Grabber's only
// view of a page. Untrusted input: any bytes give a result, never more than the caps allow.
namespace ldm::grabber {

// How a page refers to an address, which says what the address probably is.
enum class LinkKind {
    Anchor,    // <a>, <area>, <link> other than a stylesheet or icon: a page or a file
    Embedded,  // shown or used by the page itself: images, media, stylesheets, scripts, icons
    Frame,     // <frame>, <iframe>: a page
    Refresh,   // <meta http-equiv="refresh">: a page
    Form,      // <form action>: a page
};

struct HtmlLink {
    std::string address;  // as written, entities decoded, surrounding spaces removed; never empty
    LinkKind kind = LinkKind::Anchor;
    // An anchor's text, or else an image's alt text: spaces collapsed, at most kMaxLinkText bytes.
    std::string text;
    // The address's bytes in the page as written, for RewriteLinks.
    std::size_t begin = 0;
    std::size_t end = 0;
};

// A stretch of the page's bytes, as positions in it.
struct TextRange {
    std::size_t begin = 0;
    std::size_t end = 0;
};

struct PageLinks {
    std::optional<std::string> baseHref;  // the first <base href>, entities decoded
    // Where that <base href>'s value is in the page, for RewriteLinks: a saved page must not keep
    // a base that sends its converted links back to the website.
    std::optional<TextRange> baseHrefRange;
    std::vector<HtmlLink> links;  // in page order
    bool truncated = false;       // more links than the cap; the rest were not read
};

inline constexpr std::size_t kMaxLinkText = 256;

// Scans `html` for the addresses it links to: href, src, srcset, poster, data and background
// attributes of the elements that load or link something, <meta> refreshes and <form> actions.
// Comments and the contents of <script> and <style> are skipped; javascript:, mailto:, data:,
// tel: and same-page "#..." links are dropped. Stops after `maxLinks` links.
[[nodiscard]] PageLinks ScanLinks(std::string_view html, std::size_t maxLinks);

// One address of a page replaced, by its position from ScanLinks.
struct LinkReplacement {
    std::size_t begin = 0;
    std::size_t end = 0;
    // Written as is: the caller makes it safe in any attribute, quoted or not (percent-encoded,
    // with no spaces, quotes, commas or '<', '>', and '&' only as "&amp;").
    std::string address;
};

// The page with the given addresses replaced (saved pages for offline browsing, checklist G-02).
// Replacements that overlap one before them or lie outside the page are ignored.
[[nodiscard]] std::string RewriteLinks(std::string_view html, std::vector<LinkReplacement> replacements);

// Decodes HTML character references: named ones for the characters links use, and numeric ones.
// Unknown references stay as written.
[[nodiscard]] std::string DecodeEntities(std::string_view text);

}  // namespace ldm::grabber
