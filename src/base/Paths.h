#pragma once

#include <filesystem>
#include <string>

namespace ldm::base {

// Returns the `\\?\` (or `\\?\UNC\`) form of an absolute path for Win32 file APIs, so paths longer
// than MAX_PATH work without the system-wide long path setting. Relative paths are returned as is.
[[nodiscard]] std::wstring ToExtendedLengthPath(const std::filesystem::path& path);

// A key under which two paths compare equal exactly when Windows treats them as the same file name:
// normalized, with preferred separators and upper-cased as NTFS does.
[[nodiscard]] std::wstring PathComparisonKey(const std::filesystem::path& path);

}  // namespace ldm::base
