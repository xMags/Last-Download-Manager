#pragma once

#include <string>
#include <string_view>

namespace ldm::ui {

// The name Windows shows for files with `extension` (without the dot; empty for none), such as
// "MP4 File"; empty when Windows has none. Read from the file type's registration only: no file is
// opened, so a large program or a network folder can never block the window.
[[nodiscard]] std::wstring FileTypeName(std::wstring_view extension);

}  // namespace ldm::ui
