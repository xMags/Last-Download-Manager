#include "ui/History.h"

#include <algorithm>

namespace ldm::ui {

namespace {

bool EqualsIgnoreCase(std::string_view a, std::string_view b)
{
    const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) { return lower(x) == lower(y); });
}

}  // namespace

std::vector<std::string> HistoryEntries(std::string_view stored)
{
    std::vector<std::string> entries;
    size_t pos = 0;
    while (pos < stored.size()) {
        size_t end = stored.find('\n', pos);
        if (end == std::string_view::npos) {
            end = stored.size();
        }
        std::string_view line = stored.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (!line.empty()) {
            entries.emplace_back(line);
        }
        pos = end + 1;
    }
    return entries;
}

std::string PushHistory(std::string_view stored, std::string_view entry, size_t limit)
{
    if (entry.empty() || entry.find_first_of("\r\n") != std::string_view::npos) {
        return std::string(stored);
    }
    std::vector<std::string> entries = HistoryEntries(stored);
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const std::string& e) { return EqualsIgnoreCase(e, entry); }),
                  entries.end());
    entries.insert(entries.begin(), std::string(entry));
    if (entries.size() > limit) {
        entries.resize(limit);
    }
    std::string result;
    for (const std::string& e : entries) {
        if (!result.empty()) {
            result += '\n';
        }
        result += e;
    }
    return result;
}

}  // namespace ldm::ui
