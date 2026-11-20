#pragma once

#include "ui/Handles.h"

#include <windows.h>

#include <string>
#include <string_view>

namespace ldm::ui {

// What Windows shows for files with `extension` (without the dot; empty for none), read from the
// file type's registration only: no file is opened, so a large program or a network folder can
// never block the window.

// The type's icon, from the shell's image list closest to `sizePx` (48 or 256 pixels); null when
// Windows has none.
[[nodiscard]] UniqueIcon FileTypeIcon(std::wstring_view extension, int sizePx);

// The type's name, such as "MP4 File"; empty when Windows has none.
[[nodiscard]] std::wstring FileTypeName(std::wstring_view extension);

}  // namespace ldm::ui
