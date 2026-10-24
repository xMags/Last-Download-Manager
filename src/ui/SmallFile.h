#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace ldm::ui {

// Reads the whole file when it is at most `limit` bytes; nothing when it is larger or unreadable.
// Blocks while it reads, so callers on the UI thread keep `limit` small.
[[nodiscard]] std::optional<std::string> ReadSmallFile(const std::filesystem::path& path, size_t limit);

}  // namespace ldm::ui
