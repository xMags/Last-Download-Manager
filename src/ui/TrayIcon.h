#pragma once

#include "model/Settings.h"
#include "ui/Handles.h"

#include <windows.h>

#include <string>

namespace ldm::ui {

// Posted by the shell to the owner window for tray icon mouse events.
inline constexpr UINT kTrayMessage = WM_APP + 3;

// The notification area icon (checklist V-01). The owner window receives kTrayMessage and
// recreates the icon when Explorer restarts (the "TaskbarCreated" message).
class TrayIcon {
public:
    TrayIcon() = default;
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;
    ~TrayIcon();

    void Attach(HWND owner);
    void SetStyle(model::TrayIconStyle style);
    void SetTooltip(const std::wstring& tooltip);
    // After Explorer restarted.
    void Recreate();
    [[nodiscard]] static UINT TaskbarCreatedMessage();

private:
    void Add();
    void Remove();
    void Update();

    HWND owner_ = nullptr;
    model::TrayIconStyle style_ = model::TrayIconStyle::Color;
    std::wstring tooltip_ = L"Last Download Manager";
    UniqueIcon icon_;
    bool added_ = false;
};

}  // namespace ldm::ui
