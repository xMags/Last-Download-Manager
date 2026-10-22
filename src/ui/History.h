#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Recently used values stored as one setting, newest first, one per line (checklist A-01, A-02).
namespace ldm::ui {

[[nodiscard]] std::vector<std::string> HistoryEntries(std::string_view stored);

// Puts `entry` first, removes an older copy (case-insensitive for ASCII) and keeps at most
// `limit` entries. Entries containing line breaks are not stored.
[[nodiscard]] std::string PushHistory(std::string_view stored, std::string_view entry, size_t limit);

}  // namespace ldm::ui
