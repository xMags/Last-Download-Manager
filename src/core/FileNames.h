#pragma once

#include <string>
#include <string_view>

namespace ldm::core {

// File name suggested by an address: the last path segment, percent-decoded. Empty when the path
// names no file (for example "https://example.com/").
[[nodiscard]] std::string FileNameFromUrlPath(std::string_view path);

// Makes a UTF-8 name safe as a single Windows file name: path separators and reserved characters
// become '_', control characters are removed, trailing dots and spaces are trimmed, device names
// (CON, NUL, COM1, ...) are prefixed, invalid UTF-8 is replaced and the length is capped while
// keeping the extension. Never returns an empty name.
[[nodiscard]] std::string SanitizeFileName(std::string_view name);

// Lower-case extension without the dot ("tar.gz" yields "gz"); empty when there is none.
[[nodiscard]] std::string FileExtension(std::string_view name);

// "name.ext" -> "name (n).ext", used to avoid existing files (checklist A-05, A-06).
[[nodiscard]] std::string NumberedFileName(std::string_view name, int number);

}  // namespace ldm::core
