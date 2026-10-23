#include "ui/PageLinks.h"

#include "core/FileNames.h"
#include "core/Url.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace ldm::ui {

namespace {

// Extensions of addresses that usually answer with a web page.
constexpr std::array<std::string_view, 10> kPageExtensions = {"htm",  "html", "shtml", "xhtml", "php",
                                                             "asp",  "aspx", "jsp",   "cgi",   "pl"};

std::string WithoutFragment(const std::string& url)
{
    const size_t hash = url.find('#');
    return hash == std::string::npos ? url : url.substr(0, hash);
}

std::string Lower(std::string_view text)
{
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

// The name in the address, or empty when the address names no file ("https://a.test/dir/").
std::string NameFromAddress(const std::string& url)
{
    const auto parsed = core::ParseUrl(url);
    if (!parsed) {
        return {};
    }
    const std::string name = core::FileNameFromUrlPath(parsed->path);
    return name.empty() ? std::string() : core::SanitizeFileName(name);
}

}  // namespace

std::vector<LinkRow> MakeLinkRows(const browser::Links& links, const std::set<std::string>& listed)
{
    std::vector<LinkRow> rows;
    rows.reserve(links.links.size());
    for (const browser::PageLink& link : links.links) {
        LinkRow row;
        row.link = link;
        if (link.fileName) {
            row.chosenName = core::SanitizeFileName(*link.fileName);
            row.fileName = *row.chosenName;
        } else {
            row.fileName = NameFromAddress(link.url);
        }
        row.checked = links.selection;
        row.inDownloadList = listed.contains(link.url);
        rows.push_back(std::move(row));
    }
    return rows;
}

bool ApplyLinkDetails(std::vector<LinkRow>& rows, const browser::LinkDetails& details)
{
    if (details.index >= rows.size()) {
        return false;
    }
    LinkRow& row = rows[details.index];
    if (details.failed) {
        row.check = LinkCheck::Failed;
        return true;
    }
    row.check = LinkCheck::Done;
    row.size = details.fileSize;
    if (details.mimeType) {
        // "text/html; charset=utf-8" -> "text/html"
        std::string type = Lower(details.mimeType->substr(0, details.mimeType->find(';')));
        type.erase(std::remove_if(type.begin(), type.end(), [](unsigned char c) { return std::isspace(c) != 0; }),
                   type.end());
        row.mimeType = type.empty() ? std::nullopt : std::optional<std::string>(type);
    }
    // The server's name wins over the address's, as it will when downloading; a name the page or
    // the user chose stays.
    if (details.fileName && !row.chosenName) {
        row.fileName = core::SanitizeFileName(*details.fileName);
    }
    return true;
}

bool IsHtmlLink(const LinkRow& row)
{
    if (row.mimeType) {
        return *row.mimeType == "text/html" || *row.mimeType == "application/xhtml+xml";
    }
    if (row.link.kind == browser::LinkKind::Frame) {
        return true;
    }
    if (row.fileName.empty()) {
        return true;  // a folder or a site's front page
    }
    const std::string extension = core::FileExtension(row.fileName);
    return std::find(kPageExtensions.begin(), kPageExtensions.end(), extension) != kPageExtensions.end();
}

std::vector<size_t> VisibleLinkRows(std::span<const LinkRow> rows, const LinkFilter& filter)
{
    std::vector<size_t> visible;
    std::set<std::string> seen;
    for (size_t i = 0; i < rows.size(); ++i) {
        const LinkRow& row = rows[i];
        const bool repeated = !seen.insert(WithoutFragment(row.link.url)).second || row.inDownloadList;
        if ((filter.hideHtml && IsHtmlLink(row)) || (filter.hideImages && row.link.kind == browser::LinkKind::Image) ||
            (filter.hideRepeated && repeated)) {
            continue;
        }
        visible.push_back(i);
    }
    return visible;
}

std::string LinkTypeText(const LinkRow& row)
{
    std::string extension = core::FileExtension(row.fileName);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (extension.empty() && IsHtmlLink(row)) {
        return "HTML";
    }
    return extension;
}

std::optional<std::string> NameFromPattern(std::string_view pattern, size_t number, size_t count,
                                           std::string_view originalName)
{
    const size_t star = pattern.find('*');
    if (star == std::string_view::npos || pattern.find('*', star + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    std::string digits = std::to_string(number);
    const size_t width = std::to_string(std::max(count, number)).size();
    digits.insert(0, width - digits.size(), '0');
    std::string name(pattern.substr(0, star));
    name += digits;
    const std::string_view after = pattern.substr(star + 1);
    name += after;
    // "photo*.jpg" names the extension; "photo*" keeps the file's own, as it was written.
    const size_t dot = originalName.rfind('.');
    if (after.find('.') == std::string_view::npos && dot != std::string_view::npos && dot > 0 &&
        dot + 1 < originalName.size()) {
        name += originalName.substr(dot);
    }
    return core::SanitizeFileName(name);
}

std::vector<model::Cookie> CookiesForAddress(std::span<const model::Cookie> cookies, const std::string& url)
{
    const auto parsed = core::ParseUrl(url);
    if (!parsed) {
        return {};
    }
    std::vector<model::Cookie> matching;
    for (const model::Cookie& cookie : cookies) {
        if (model::CookieApplies(cookie, parsed->scheme, parsed->host, parsed->path)) {
            matching.push_back(cookie);
        }
    }
    return matching;
}

}  // namespace ldm::ui
