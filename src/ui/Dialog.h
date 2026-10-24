#pragma once

#include "ui/Theme.h"

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>

namespace ldm::ui {

// Base for DIALOGEX-template dialogs (design section 6): routes messages to virtual handlers,
// applies the theme, and offers typed control helpers. Modal through RunModal(); modeless through
// CreateModeless() with the caller keeping the object alive until the window is destroyed.
class Dialog {
public:
    Dialog(int templateId, const Theme& theme) : templateId_(templateId), theme_(theme) {}
    virtual ~Dialog();
    Dialog(const Dialog&) = delete;
    Dialog& operator=(const Dialog&) = delete;

    // Returns the code passed to EndDialog (IDOK, IDCANCEL, ...).
    INT_PTR RunModal(HWND owner);
    HWND CreateModeless(HWND owner);
    [[nodiscard]] HWND Handle() const noexcept { return window_; }

protected:
    virtual bool OnInit() { return true; }
    // Return true when handled. The default closes on IDOK and IDCANCEL.
    virtual bool OnCommand(int id, int code, HWND control);
    virtual bool OnNotify(const NMHDR& header, LRESULT& result)
    {
        (void)header;
        (void)result;
        return false;
    }
    // Any other message; return true with `result` set when handled.
    virtual bool OnMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
    {
        (void)message;
        (void)wParam;
        (void)lParam;
        (void)result;
        return false;
    }
    // Modeless dialogs only: OnDestroying runs while the controls still exist; OnDestroyed runs
    // after the window is gone and may delete the object.
    virtual void OnDestroying() {}
    virtual void OnDestroyed() {}

    void Close(INT_PTR result);
    [[nodiscard]] bool IsModal() const noexcept { return modal_; }
    [[nodiscard]] const Theme& CurrentTheme() const noexcept { return theme_; }

    [[nodiscard]] HWND Item(int id) const;
    [[nodiscard]] std::wstring Text(int id) const;
    void SetText(int id, const std::wstring& text);
    [[nodiscard]] bool Checked(int id) const;
    void SetChecked(int id, bool checked);
    void Enable(int id, bool enabled);
    void Show(int id, bool visible);
    [[nodiscard]] std::optional<int> Number(int id) const;
    void SetNumber(int id, std::int64_t value);
    // Shows an error balloon-free message box owned by the dialog.
    void Warn(const std::wstring& message) const;

private:
    static INT_PTR CALLBACK DialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static void BringToFront(HWND window);
    INT_PTR Dispatch(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    int templateId_;
    const Theme& theme_;
    HWND window_ = nullptr;
    bool modal_ = false;
    // A modeless window's handler can run a nested message loop (a question, a file picker, a
    // popup menu), during which something else may destroy the window, such as LDM exiting.
    // OnDestroyed may delete this object, so while any handler of this window is still on the
    // stack it waits until the outermost one returns. Handle() is null from the destruction on.
    int dispatchDepth_ = 0;
    bool destroyPending_ = false;
};

}  // namespace ldm::ui
