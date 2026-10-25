#include "ui/WindowMoveWatch.h"

#include <utility>

namespace ldm::ui {

namespace {

// The watch of this thread: WinEvent callbacks carry no pointer of their own.
thread_local WindowMoveWatch* current = nullptr;

}  // namespace

WindowMoveWatch::WindowMoveWatch(std::function<void()> moved) : moved_(std::move(moved))
{
    current = this;
}

WindowMoveWatch::~WindowMoveWatch()
{
    Stop();
    if (current == this) {
        current = nullptr;
    }
}

void WindowMoveWatch::Watch(HWND window)
{
    if (window == window_ && (window == nullptr || hook_ != nullptr)) {
        return;
    }
    Stop();
    if (window == nullptr) {
        return;
    }
    DWORD process = 0;
    ::GetWindowThreadProcessId(window, &process);
    if (process == 0) {
        return;
    }
    // Only that process's events: a browser reports many location changes (carets, menus), and
    // OnEvent keeps those of the one window.
    hook_ = ::SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, &WindowMoveWatch::OnEvent,
                              process, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    window_ = hook_ != nullptr ? window : nullptr;
}

void WindowMoveWatch::Stop()
{
    if (hook_ != nullptr) {
        ::UnhookWinEvent(hook_);
        hook_ = nullptr;
    }
    window_ = nullptr;
}

void CALLBACK WindowMoveWatch::OnEvent(HWINEVENTHOOK hook, DWORD, HWND window, LONG objectId, LONG, DWORD, DWORD)
{
    WindowMoveWatch* watch = current;
    // An event queued before the hook changed may still arrive; only the current hook's count.
    if (watch == nullptr || hook != watch->hook_ || window != watch->window_ || objectId != OBJID_WINDOW) {
        return;
    }
    if (watch->moved_) {
        watch->moved_();
    }
}

}  // namespace ldm::ui
