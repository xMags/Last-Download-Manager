#pragma once

#include "ui/Handles.h"

#include <windows.h>

#include <array>
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

// Toolbar icons as thin outlines filled with a diagonal two-color gradient, built
// from one or two glyphs (for example a ring with an arrow inside) or from a drawn shape.
struct IconLayer {
    wchar_t codepoint = 0;   // 0: unused
    int scalePercent = 100;  // glyph size relative to the icon
    int offsetXPercent = 0;  // shift relative to the icon size
    int offsetYPercent = 0;
};

// Shapes the icon font lacks in the needed form, drawn as outlines under any glyph layers.
enum class IconShape {
    Glyphs,
    Ring,  // a thin circle, for a glyph drawn inside it
    RaisedHand,
    OutlinePlus,
    Trash,       // with the two lines in the bin
    EmptyTrash,  // a plain bin, for a glyph drawn inside it
};

struct GradientIcon {
    std::array<IconLayer, 2> layers{};
    COLORREF start = 0;  // top left
    COLORREF end = 0;    // bottom right
    IconShape shape = IconShape::Glyphs;
};

// One premultiplied 32-bit image per icon, `sizePx` square. With `grayed`, a flat light gray, for
// disabled buttons.
[[nodiscard]] UniqueImageList CreateGradientImageList(std::span<const GradientIcon> icons, int sizePx, bool grayed);

// A single glyph as an icon, for windows and tray entries that need an HICON.
[[nodiscard]] UniqueIcon CreateGlyphIcon(const Glyph& glyph, int sizePx);

// The icon font itself, glyphs `sizePx` high, for drawing glyphs as text in any color.
[[nodiscard]] UniqueFont CreateIconFont(int sizePx);

}  // namespace ldm::ui
