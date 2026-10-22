#pragma once

#include "ui/Theme.h"

#include <windows.h>

#include <string>
#include <vector>

namespace ldm::ui {

// A painted status bar (checklist W-01). A custom window instead of the common control, so it
// follows the dark palette without owner-draw tricks.
class StatusBar {
public:
    StatusBar() = default;
    StatusBar(const StatusBar&) = delete;
    StatusBar& operator=(const StatusBar&) = delete;
    ~StatusBar();

    void Create(HWND parent, const Theme& theme);
    [[nodiscard]] HWND Handle() const noexcept { return window_; }
    [[nodiscard]] int PreferredHeight() const;

    void SetTheme(const Theme& theme);
    void SetFont(HFONT font);
    // Text segments, left to right, separated by thin dividers.
    void SetParts(std::vector<std::wstring> parts);

private:
    static void RegisterClassOnce();
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void Paint();

    HWND window_ = nullptr;
    const Theme* theme_ = nullptr;
    HFONT font_ = nullptr;
    std::vector<std::wstring> parts_;
};

}  // namespace ldm::ui
