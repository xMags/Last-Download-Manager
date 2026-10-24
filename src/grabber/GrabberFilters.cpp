#include "grabber/GrabberFilters.h"

#include "core/FileNames.h"
#include "core/Matching.h"
#include "core/NetworkAddress.h"

#include <algorithm>
#include <array>
#include <vector>

namespace ldm::grabber {

namespace {

constexpr std::array<std::string_view, 14> kPageExtensions{"htm",  "html", "xhtml", "shtml", "php", "php3", "asp",
                                                           "aspx", "jsp",  "jspx",  "cfm",   "cgi", "pl",   "do"};

char ToLowerAscii(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// A mask with '*' matches the whole path; one without matches the paths that start with it, so
// "/docs/" stands for everything below that folder.
bool PathMatches(std::string_view mask, std::string_view path)
{
    if (mask.find('*') != std::string_view::npos) {
        return core::WildcardMatch(mask, path);
    }
    return path.size() >= mask.size() && core::WildcardMatch(mask, path.substr(0, mask.size()));
}

bool AnyPathMatches(const std::vector<std::string>& masks, std::string_view path)
{
    return std::any_of(masks.begin(), masks.end(), [path](const std::string& mask) { return PathMatches(mask, path); });
}

bool PassesPathMasks(const std::vector<std::string>& include, const std::vector<std::string>& exclude,
                     std::string_view path)
{
    if (!include.empty() && !AnyPathMatches(include, path)) {
        return false;
    }
    return !AnyPathMatches(exclude, path);
}

bool AnyNameMatches(const std::vector<std::string>& masks, std::string_view name)
{
    return std::any_of(masks.begin(), masks.end(),
                       [name](const std::string& mask) { return core::WildcardMatch(mask, name); });
}

bool IsAddressLiteral(std::string_view host)
{
    return core::ParseIpv4(host).has_value() || host.find(':') != std::string_view::npos;
}

std::vector<std::string_view> Labels(std::string_view host)
{
    std::vector<std::string_view> labels;
    std::size_t start = 0;
    while (start <= host.size()) {
        const std::size_t dot = host.find('.', start);
        const std::size_t end = dot == std::string_view::npos ? host.size() : dot;
        labels.push_back(host.substr(start, end - start));
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    return labels;
}

}  // namespace

std::string MainDomainOf(std::string_view host)
{
    if (IsAddressLiteral(host)) {
        return std::string(host);
    }
    const std::vector<std::string_view> labels = Labels(host);
    if (labels.size() <= 2) {
        return std::string(host);
    }
    std::size_t keep = 2;
    const std::string_view last = labels[labels.size() - 1];
    const std::string_view secondLast = labels[labels.size() - 2];
    if (last.size() == 2 && secondLast.size() <= 3 && labels.size() >= 3) {
        keep = 3;
    }
    std::string domain;
    for (std::size_t i = labels.size() - keep; i < labels.size(); ++i) {
        if (!domain.empty()) {
            domain.push_back('.');
        }
        domain.append(labels[i]);
    }
    return domain;
}

BaseSite BaseSiteOf(const core::Url& startPage)
{
    BaseSite site;
    site.host = startPage.host;
    site.mainDomain = MainDomainOf(startPage.host);
    const std::size_t slash = startPage.path.rfind('/');
    site.folder = slash == std::string::npos ? "/" : startPage.path.substr(0, slash + 1);
    return site;
}

bool OnBaseSite(const BaseSite& site, const core::Url& url, bool mainDomainSites)
{
    if (url.host == site.host) {
        return true;
    }
    if (!mainDomainSites || IsAddressLiteral(url.host)) {
        return false;
    }
    return url.host == site.mainDomain ||
           (url.host.size() > site.mainDomain.size() + 1 && url.host.ends_with(site.mainDomain) &&
            url.host[url.host.size() - site.mainDomain.size() - 1] == '.');
}

PageDepth DepthOfLink(const PageDepth& parent, bool onBaseSite)
{
    return PageDepth{parent.depth + 1, onBaseSite ? 0 : parent.otherSitesDepth + 1};
}

bool ShouldExplore(const model::GrabberExploreFilters& filters, const BaseSite& site, const core::Url& url,
                   PageDepth depth)
{
    if (!PassesPathMasks(filters.includePaths, filters.excludePaths, url.path)) {
        return false;
    }
    const bool onBase = OnBaseSite(site, url, filters.mainDomainSites);
    if (!onBase) {
        return depth.otherSitesDepth <= filters.otherSitesDepth;
    }
    if (filters.baseSiteDepth && depth.depth > *filters.baseSiteDepth) {
        return false;
    }
    if (filters.noParentDirectories && url.host == site.host) {
        return url.path.size() >= site.folder.size() &&
               std::equal(site.folder.begin(), site.folder.end(), url.path.begin(),
                          [](char a, char b) { return ToLowerAscii(a) == ToLowerAscii(b); });
    }
    return true;
}

FileMatch MatchFile(const model::GrabberFileFilters& files, const BaseSite& site, bool mainDomainSites,
                    const core::Url& url, std::optional<std::int64_t> size)
{
    if (files.thisSiteOnly && !OnBaseSite(site, url, mainDomainSites)) {
        return FileMatch::Filtered;
    }
    if (!PassesPathMasks(files.includePaths, files.excludePaths, url.path)) {
        return FileMatch::Filtered;
    }
    const std::string name = core::FileNameFromUrlPath(url.path);
    if (!files.includeTypes.empty() && !AnyNameMatches(files.includeTypes, name)) {
        return FileMatch::Filtered;
    }
    if (AnyNameMatches(files.excludeTypes, name)) {
        return FileMatch::Filtered;
    }
    if (!files.minBytes && !files.maxBytes) {
        return FileMatch::Match;
    }
    if (!size) {
        return FileMatch::NeedsSize;
    }
    if ((files.minBytes && *size < *files.minBytes) || (files.maxBytes && *size > *files.maxBytes)) {
        return FileMatch::Filtered;
    }
    return FileMatch::Match;
}

std::string NormalizeAddress(const core::Url& url)
{
    std::string address = url.scheme + "://";
    address += url.host.find(':') != std::string::npos ? "[" + url.host + "]" : url.host;
    const int defaultPort = url.scheme == "https" ? 443 : url.scheme == "http" ? 80 : url.scheme == "ftp" ? 21 : 990;
    if (url.port && *url.port != defaultPort) {
        address += ":" + std::to_string(*url.port);
    }
    address += url.path;
    if (!url.query.empty()) {
        address += "?" + url.query;
    }
    return address;
}

bool LooksLikePage(const core::Url& url)
{
    if (url.path.ends_with('/')) {
        return true;
    }
    const std::string name = core::FileNameFromUrlPath(url.path);
    const std::string extension = core::FileExtension(name);
    if (extension.empty()) {
        return true;
    }
    return std::find(kPageExtensions.begin(), kPageExtensions.end(), extension) != kPageExtensions.end();
}

bool IsPageType(std::string_view contentType)
{
    const std::size_t semicolon = contentType.find(';');
    std::string_view type = contentType.substr(0, semicolon);
    while (!type.empty() && (type.back() == ' ' || type.back() == '\t')) {
        type.remove_suffix(1);
    }
    while (!type.empty() && (type.front() == ' ' || type.front() == '\t')) {
        type.remove_prefix(1);
    }
    std::string lower(type);
    std::transform(lower.begin(), lower.end(), lower.begin(), ToLowerAscii);
    return lower == "text/html" || lower == "application/xhtml+xml";
}

}  // namespace ldm::grabber
