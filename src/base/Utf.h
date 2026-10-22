#pragma once

#include <string>
#include <string_view>

namespace ldm::base {

// Conversions between UTF-8 (used internally, in the database and in IPC) and UTF-16
// (used by Win32). Invalid sequences are replaced with U+FFFD rather than failing.
[[nodiscard]] std::wstring ToUtf16(std::string_view utf8);
[[nodiscard]] std::string ToUtf8(std::wstring_view utf16);

}  // namespace ldm::base
