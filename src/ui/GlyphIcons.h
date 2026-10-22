#pragma once

#include "ui/Handles.h"

#include <windows.h>

#include <span>

namespace ldm::ui {

// One icon drawn from the Windows icon font (Segoe Fluent Icons on Windows 11, Segoe MDL2 Assets on
// Windows 10). Glyphs scale to any DPI without bitmaps, and the font ships with Windows.
struct Glyph {
    wchar_t codepoint = 0;
    COLORREF color = 0;
};

// Builds a 32-bit image list with one premultiplied-alpha image per glyph, `sizePx` square.
// With `grayed`, every glyph is drawn in a neutral gray (for disabled toolbar buttons).
[[nodiscard]] UniqueImageList CreateGlyphImageList(std::span<const Glyph> glyphs, int sizePx, bool grayed = false);

// A single glyph as an icon, for windows and tray entries that need an HICON.
[[nodiscard]] UniqueIcon CreateGlyphIcon(const Glyph& glyph, int sizePx);

}  // namespace ldm::ui
