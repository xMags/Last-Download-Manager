#pragma once

#include "core/Snapshot.h"
#include "ui/Dialog.h"
#include "ui/Handles.h"
#include "ui/ShellActions.h"

#include <functional>
#include <string>

namespace ldm::ui {

// Download complete (checklist C-01): open the file, open it with another program, show it in its
// folder, or drag its icon to Explorer or another program. Modeless; several can be open.
class CompleteDialog final : public Dialog {
public:
    struct Callbacks {
        std::function<void()> dontShowAgain;
        std::function<void(CompleteDialog*)> closed;  // the owner deletes the object
    };

    CompleteDialog(const Theme& theme, ShellActions& shell, core::DownloadCompleted completed, Callbacks callbacks);
    void Open(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;
    void OnDestroying() override;
    void OnDestroyed() override;
    static LRESULT CALLBACK IconProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data);
    void StartDrag();

    ShellActions& shell_;
    core::DownloadCompleted completed_;
    Callbacks callbacks_;
    UniqueIcon icon_;
};

// The cancellable countdown before a power action after a download completes (checklist P-05).
class CountdownDialog final : public Dialog {
public:
    CountdownDialog(const Theme& theme, std::wstring action, int seconds);
    // True when the action should run (the countdown finished or the user chose to do it now).
    [[nodiscard]] bool Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;
    bool OnMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) override;
    void Refresh();

    std::wstring action_;
    int remaining_;
    bool confirmed_ = false;
};

}  // namespace ldm::ui
