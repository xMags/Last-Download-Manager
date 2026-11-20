#pragma once

#include "model/Category.h"

#include <string_view>
#include <vector>

namespace ldm::ui {

// The kinds of file LDM's built-in categories hold; Other for any other category or file.
enum class FileKind {
    Other,
    Compressed,
    Documents,
    Music,
    Programs,
    Video,
};

// A built-in category's kind, by the name it was seeded with; Other for a renamed or user category.
[[nodiscard]] FileKind KindOfCategory(const model::Category& category);

// The kind of the built-in category whose extensions include `fileName`'s (letter case does not
// matter); Other when none does.
[[nodiscard]] FileKind KindOfFile(std::string_view fileName, const std::vector<model::Category>& categories);

// Icon-font glyphs: a category's icon (a folder for Other), and the badge drawn on a file of the
// kind (0 for Other, which shows its extension instead).
[[nodiscard]] wchar_t CategoryGlyph(FileKind kind) noexcept;
[[nodiscard]] wchar_t FileBadgeGlyph(FileKind kind) noexcept;

}  // namespace ldm::ui
