#pragma once

#include "ui/Handles.h"
#include "ui/LinkDropTarget.h"
#include "ui/Theme.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace ldm::ui {

// The drop target (checklist W-06): a small always-on-top window that accepts links dragged from a
// browser or any other program. Drag the window itself to move it; right-click to hide it;
// double-click to open the main window.
class DropTargetWindow {
public:
    struct Callbacks {
        std::function<void(std::vector<std::string>)> dropped;  // UTF-8 addresses, in order
        std::function<void()> hideRequested;
        std::function<void()> openMain;
        std::function<void(const std::string&)> moved;  // new position, in the form Create() accepts
    };

    DropTargetWindow() = default;
    ~DropTargetWindow();
    DropTargetWindow(const DropTargetWindow&) = delete;
    DropTargetWindow& operator=(const DropTargetWindow&) = delete;

    // `position` is "x,y" in screen pixels, or empty for the default near the top right corner.
    void Create(const std::string& position, const Theme& theme, Callbacks callbacks);
    void Destroy();
    [[nodiscard]] bool IsOpen() const noexcept { return window_ != nullptr; }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void Paint();

    HWND window_ = nullptr;
    const Theme* theme_ = nullptr;
    Callbacks callbacks_;
    LinkDropTarget drop_;
    UniqueIcon icon_;
    bool hovering_ = false;
};

}  // namespace ldm::ui
