#pragma once

#include "ui/Handles.h"
#include "ui/MediaPanelMenu.h"
#include "ui/Theme.h"

#include <windows.h>

namespace ldm::ui {

// Draws the media panel's menu (owner-drawn lines of a TrackPopupMenu menu): an icon column,
// the text with its details quieter, a badge such as "4K", the size on the right and a rounded
// highlight. Each menu line's item data is the PanelMenuItem it shows, which must outlive the
// menu. Lives only while the menu is open; UI thread only.
class PanelMenuPainter {
public:
    PanelMenuPainter(const Theme& theme, UINT dpi);
    PanelMenuPainter(const PanelMenuPainter&) = delete;
    PanelMenuPainter& operator=(const PanelMenuPainter&) = delete;

    // The menu's background, also behind its edges and in its submenus.
    [[nodiscard]] HBRUSH Background() const noexcept { return background_.get(); }

    void Measure(MEASUREITEMSTRUCT& measure) const;
    void Draw(const DRAWITEMSTRUCT& draw) const;

private:
    [[nodiscard]] int Px(int value96) const noexcept;

    const Palette& colors_;
    bool dark_ = false;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    UniqueFont text_;
    UniqueFont badgeFont_;
    UniqueFont icons_;
    UniqueFont chevron_;
    UniqueBrush background_;
};

}  // namespace ldm::ui
