#pragma once

#include "ui/Handles.h"

#include <windows.h>

// Light and dark appearance (checklist V-08, design section 6).
//
// Title bars use the documented DWMWA_USE_IMMERSIVE_DARK_MODE and, on Windows 11, take the panel
// color through DWMWA_CAPTION_COLOR so title bar, menu bar and toolbar read as one surface. Dark menus, scroll bars and common
// controls need uxtheme functions that Windows exports only by ordinal; they are looked up once,
// only on builds known to have them, and every caller falls back to the light look when a lookup
// fails. The menu bar is painted by the window through the UAH messages handled here.
namespace ldm::ui {

struct Palette {
    COLORREF window;      // list, tree and dialog background
    COLORREF panel;       // toolbar, status bar and menu bar
    COLORREF text;
    COLORREF mutedText;
    COLORREF border;
    COLORREF hot;         // menu bar item under the mouse
    COLORREF accent;      // progress fill
    COLORREF accentText;
    COLORREF divider;     // hairlines between the header, panes and list
};

class Theme {
public:
    // Call once before creating windows. Enables dark popup menus for the process when allowed.
    static void InitializeProcess();

    // `settingEnabled` is View > Dark mode support; dark is used only when Windows apps are dark too.
    explicit Theme(bool settingEnabled);

    // Re-reads the Windows app mode; true when the effective appearance changed.
    bool Refresh(bool settingEnabled);

    [[nodiscard]] bool IsDark() const noexcept { return dark_; }
    [[nodiscard]] const Palette& Colors() const noexcept { return palette_; }
    [[nodiscard]] HBRUSH WindowBrush() const noexcept { return windowBrush_.get(); }
    [[nodiscard]] HBRUSH PanelBrush() const noexcept { return panelBrush_.get(); }

    // Title bar and non-client colors of a top-level window or dialog.
    void ApplyToTopLevel(HWND window) const;
    // Scroll bars and themed parts of a list view, tree view, edit, combo box or button.
    void ApplyToControl(HWND control) const;
    // Applies ApplyToControl to every child of `parent`.
    void ApplyToChildren(HWND parent) const;

    // WM_CTLCOLOR* for dialogs: sets the DC colors and returns the brush to paint with.
    [[nodiscard]] HBRUSH OnCtlColor(HDC dc, bool editable) const;

    // Handles WM_UAHDRAWMENU, WM_UAHDRAWMENUITEM and the line under the menu bar, painting the menu
    // bar in the panel color in both light and dark mode.
    // Returns true when the message was fully handled and `result` is set.
    bool HandleMenuBarMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) const;

private:
    void BuildPalette();

    bool dark_ = false;
    Palette palette_{};
    UniqueBrush windowBrush_;
    UniqueBrush panelBrush_;
    UniqueBrush hotBrush_;
};

}  // namespace ldm::ui
