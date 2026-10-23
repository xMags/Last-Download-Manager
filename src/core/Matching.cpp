#include "core/Matching.h"

#include "core/FileNames.h"

#include <algorithm>
#include <vector>

namespace ldm::core {

namespace {

char ToLowerAscii(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Splits a space-separated list, skipping empty entries.
std::vector<std::string_view> Words(std::string_view list)
{
    std::vector<std::string_view> words;
    size_t pos = 0;
    while (pos < list.size()) {
        const size_t start = list.find_first_not_of(" \t", pos);
        if (start == std::string_view::npos) {
            break;
        }
        const size_t end = list.find_first_of(" \t", start);
        words.push_back(list.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        pos = end == std::string_view::npos ? list.size() : end;
    }
    return words;
}

}  // namespace

bool WildcardMatch(std::string_view pattern, std::string_view text)
{
    // Iterative matcher with single backtracking point: linear in practice, no recursion.
    size_t p = 0;
    size_t t = 0;
    size_t starPattern = std::string_view::npos;
    size_t starText = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            starPattern = p++;
            starText = t;
        } else if (p < pattern.size() && ToLowerAscii(pattern[p]) == ToLowerAscii(text[t])) {
            ++p;
            ++t;
        } else if (starPattern != std::string_view::npos) {
            p = starPattern + 1;
            t = ++starText;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

bool MatchesExtensionList(std::string_view list, std::string_view extension)
{
    if (extension.empty()) {
        return false;
    }
    for (std::string_view entry : Words(list)) {
        if (!entry.empty() && entry.front() == '.') {
            entry.remove_prefix(1);
        }
        if (WildcardMatch(entry, extension)) {
            return true;
        }
    }
    return false;
}

std::optional<model::CategoryId> MatchCategory(std::span<const model::Category> categories, std::string_view fileName,
                                               const Url& url)
{
    for (const model::Category& category : categories) {
        for (const std::string_view site : Words(category.sites)) {
            const bool hasPath = site.find('/') != std::string_view::npos;
            if (WildcardMatch(site, hasPath ? url.host + url.path : url.host)) {
                return category.id;
            }
        }
    }
    const std::string extension = FileExtension(fileName);
    if (extension.empty()) {
        return std::nullopt;
    }
    for (const model::Category& category : categories) {
        for (std::string_view candidate : Words(category.extensions)) {
            if (!candidate.empty() && candidate.front() == '.') {
                candidate.remove_prefix(1);
            }
            if (candidate.size() == extension.size() && WildcardMatch(candidate, extension)) {
                return category.id;
            }
        }
    }
    return std::nullopt;
}

std::optional<int> MatchServerLimit(std::span<const model::ServerLimit> limits, std::string_view host)
{
    const model::ServerLimit* best = nullptr;
    bool bestExact = false;
    for (const model::ServerLimit& limit : limits) {
        if (!WildcardMatch(limit.pattern, host)) {
            continue;
        }
        const bool exact = limit.pattern.find('*') == std::string::npos;
        if (best == nullptr || (exact && !bestExact) ||
            (exact == bestExact && limit.pattern.size() > best->pattern.size())) {
            best = &limit;
            bestExact = exact;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    return best->maxConnections;
}

const model::SiteLogin* MatchSiteLogin(std::span<const model::SiteLogin> logins, const Url& url)
{
    const model::SiteLogin* best = nullptr;
    for (const model::SiteLogin& login : logins) {
        const std::string_view pattern = login.pattern;
        const size_t slash = pattern.find('/');
        const std::string_view hostPattern = pattern.substr(0, slash);
        if (!WildcardMatch(hostPattern, url.host)) {
            continue;
        }
        if (slash != std::string_view::npos) {
            const std::string_view prefix = pattern.substr(slash);
            if (!url.path.starts_with(prefix)) {
                continue;
            }
            // "/files" must not match "/filesystem": the prefix ends at a segment boundary.
            if (url.path.size() > prefix.size() && prefix.back() != '/' && url.path[prefix.size()] != '/') {
                continue;
            }
        }
        if (best == nullptr || login.pattern.size() > best->pattern.size()) {
            best = &login;
        }
    }
    return best;
}

}  // namespace ldm::core
