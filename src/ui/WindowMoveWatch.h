#pragma once

#include <windows.h>

#include <functional>

namespace ldm::ui {

// Calls back whenever one top-level window of another process moves or changes size, as it
// happens: a WinEvent hook out of context, which Windows delivers through this thread's message
// loop. The media panel uses it to stay on the browser window while the user drags it, instead of
// catching up at its next placement tick. UI thread only; at most one watch per thread.
class WindowMoveWatch {
public:
    explicit WindowMoveWatch(std::function<void()> moved);
    ~WindowMoveWatch();
    WindowMoveWatch(const WindowMoveWatch&) = delete;
    WindowMoveWatch& operator=(const WindowMoveWatch&) = delete;

    // Watches `window` from now on (the one before is let go); null stops watching.
    void Watch(HWND window);

private:
    static void CALLBACK OnEvent(HWINEVENTHOOK hook, DWORD event, HWND window, LONG objectId, LONG childId,
                                 DWORD thread, DWORD time);
    void Stop();

    std::function<void()> moved_;
    HWINEVENTHOOK hook_ = nullptr;
    HWND window_ = nullptr;
};

}  // namespace ldm::ui
