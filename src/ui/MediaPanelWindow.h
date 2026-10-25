#pragma once

#include "ui/Handles.h"

#include <windows.h>

#include <functional>
#include <string>

namespace ldm::ui {

// The button LDM shows over a web video (checklist B-07, M3 design decision 4), drawn by LDM: a
// borderless window that stays on top, never takes the focus from the
// browser and has no taskbar entry. Full mode shows "Download this video" with a menu arrow, a
// "make smaller" and a close button; mini mode shows only the icon and the arrow. It draws and
// reports clicks; the controller decides what the menu holds and where the window goes. While the
// controller's menu is open, the window (its owner) passes the menu's drawing to a painter.
class PanelMenuPainter;

class MediaPanelWindow {
public:
    struct Callbacks {
        std::function<void(POINT anchor)> openMenu;  // screen point under the panel's left edge
        std::function<void()> close;                  // the user closed the panel
        std::function<void()> tick;                   // the placement timer, while it runs
    };

    MediaPanelWindow() = default;
    ~MediaPanelWindow();
    MediaPanelWindow(const MediaPanelWindow&) = delete;
    MediaPanelWindow& operator=(const MediaPanelWindow&) = delete;

    [[nodiscard]] bool Create(Callbacks callbacks);
    void Destroy();

    // The panel's size with its top-left corner at `at`, at the DPI of the monitor there.
    [[nodiscard]] SIZE SizeAt(POINT at) const;
    void ShowAt(POINT topLeft);
    void Hide();
    [[nodiscard]] bool Visible() const noexcept { return visible_; }
    [[nodiscard]] HWND Handle() const noexcept { return window_; }
    [[nodiscard]] UINT Dpi() const noexcept { return dpi_; }
    // Draws the owner-drawn menu the controller opens; null when no menu is open. Not owned.
    void SetMenuPainter(const PanelMenuPainter* painter) noexcept { menuPainter_ = painter; }

    // `protectedVideo`: the button says the video is protected instead of offering a download.
    void SetContent(bool protectedVideo);
    void SetMini(bool mini);
    [[nodiscard]] bool Mini() const noexcept { return mini_; }
    // Repaints with the theme's current colors.
    void ThemeChanged();

    // Calls `tick` every `intervalMs` until StopTimer.
    void StartTimer(UINT intervalMs);
    void StopTimer();

private:
    enum class Part {
        None,
        Main,   // the button: opens the menu
        Small,  // makes the panel smaller
        Close,
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void Paint();
    [[nodiscard]] SIZE SizeForDpi(UINT dpi) const;
    [[nodiscard]] Part HitTest(POINT client) const;
    [[nodiscard]] RECT PartRect(Part part) const;
    void SetHot(Part part);
    void SetPressed(Part part);
    void UpdateFont(UINT dpi);

    HWND window_ = nullptr;
    const PanelMenuPainter* menuPainter_ = nullptr;
    Callbacks callbacks_;
    UniqueFont font_;
    UniqueFont glyphs_;      // the icon, at full size
    UniqueFont smallGlyphs_; // the menu arrow and the side buttons
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    bool visible_ = false;
    bool mini_ = false;
    bool protected_ = false;
    bool timerRunning_ = false;
    bool tracking_ = false;  // TrackMouseEvent asked for WM_MOUSELEAVE
    Part hot_ = Part::None;
    Part pressed_ = Part::None;  // the mouse went down here and is still down
};

}  // namespace ldm::ui
