#include "media/M3uList.h"

#include "media/UrlReference.h"

#include <set>

namespace ldm::media {

namespace {

std::string_view Trim(std::string_view text)
{
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const size_t last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

// "#EXTINF:-1 tvg-name="A, B",Title" names "Title": the title follows the first comma that is not
// inside a quoted attribute.
std::string TitleOf(std::string_view extinf)
{
    bool quoted = false;
    for (size_t i = 0; i < extinf.size(); ++i) {
        if (extinf[i] == '"') {
            quoted = !quoted;
        } else if (extinf[i] == ',' && !quoted) {
            return std::string(Trim(extinf.substr(i + 1)));
        }
    }
    return {};
}

}  // namespace

std::vector<M3uEntry> ParseM3uLinks(std::string_view text, std::string_view baseUrl)
{
    constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";
    if (text.substr(0, kByteOrderMark.size()) == kByteOrderMark) {
        text.remove_prefix(kByteOrderMark.size());
    }
    std::vector<M3uEntry> entries;
    std::set<std::string> seen;
    std::string title;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        std::string_view line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        start = end == std::string_view::npos ? text.size() + 1 : end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        if (line.front() == '#') {
            if (line.substr(0, 7) == "#EXT-X-") {
                return {};  // an HLS playlist: segments of one stream, not links
            }
            if (line.substr(0, 8) == "#EXTINF:") {
                title = TitleOf(line.substr(8));
            }
            continue;
        }
        auto url = ResolveLinkUrl(baseUrl, line);
        const std::string entryTitle = std::move(title);
        title.clear();
        if (!url || !seen.insert(*url).second) {
            continue;
        }
        entries.push_back(M3uEntry{std::move(*url), entryTitle});
        if (entries.size() == kMaxM3uLinks) {
            break;
        }
    }
    return entries;
}

}  // namespace ldm::media
