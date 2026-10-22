#pragma once

#include <filesystem>
#include <string>

namespace ldm::base {

// Returns the `\\?\` (or `\\?\UNC\`) form of an absolute path for Win32 file APIs, so paths longer
// than MAX_PATH work without the system-wide long path setting. Relative paths are returned as is.
[[nodiscard]] std::wstring ToExtendedLengthPath(const std::filesystem::path& path);

}  // namespace ldm::base
