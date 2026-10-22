#pragma once

#include "ui/Handles.h"

#include <windows.h>

#include <string>

// Per-Monitor V2 helpers (design section 6). Every size in code is written at 96 DPI and scaled
// at the point of use with the window's current DPI.
namespace ldm::ui {

[[nodiscard]] inline int Scale(int value96, UINT dpi) noexcept
{
    return ::MulDiv(value96, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

[[nodiscard]] inline int Unscale(int value, UINT dpi) noexcept
{
    return ::MulDiv(value, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi));
}

[[nodiscard]] inline UINT DpiOf(HWND window) noexcept
{
    const UINT dpi = ::GetDpiForWindow(window);
    return dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi;
}

// The system message font (what dialogs and lists use) at the given DPI.
[[nodiscard]] UniqueFont CreateMessageFont(UINT dpi);

// A font from "face;pointSize" (the stored list font setting, checklist L-08), or the message
// font when the text is empty or unusable.
[[nodiscard]] UniqueFont CreateFontFromSetting(const std::wstring& setting, UINT dpi);

}  // namespace ldm::ui
