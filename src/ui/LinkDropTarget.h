#pragma once

#include <windows.h>
#include <oleidl.h>

#include <functional>
#include <string>
#include <vector>

namespace ldm::ui {

// Accepts links dragged from a browser or any other program onto a window (checklist W-06): the
// drop target window and the main window. Registers the window with OLE for its lifetime.
// UI thread only; the thread must have called OleInitialize.
class LinkDropTarget {
public:
    struct Callbacks {
        // UTF-8 addresses, in order. Runs inside the source program's drag loop, so it must return
        // at once (queue the work, never open a dialog).
        std::function<void(std::vector<std::string>)> dropped;
        std::function<void(bool)> hovering;  // optional: a link is being dragged over the window
    };

    LinkDropTarget() = default;
    ~LinkDropTarget();
    LinkDropTarget(const LinkDropTarget&) = delete;
    LinkDropTarget& operator=(const LinkDropTarget&) = delete;

    // False when OLE refused the registration; the window then simply takes no drops.
    bool Register(HWND window, Callbacks callbacks);
    // Must run before the window is destroyed. Harmless when not registered.
    void Revoke();

private:
    class Handler;

    HWND window_ = nullptr;
    Handler* handler_ = nullptr;  // COM object; one reference held while registered
};

// Addresses in dragged data: the browser's URL format first, then plain text.
[[nodiscard]] std::vector<std::string> UrlsFromDataObject(IDataObject* data);

}  // namespace ldm::ui
