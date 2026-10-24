#include "ui/dialogs/CompleteDialog.h"

#include "base/Utf.h"
#include "core/FileNames.h"
#include "resource.h"
#include "ui/Format.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

namespace ldm::ui {

namespace {

constexpr UINT_PTR kIconSubclassId = 1;
constexpr UINT_PTR kCountdownTimer = 1;

}  // namespace

CompleteDialog::CompleteDialog(const Theme& theme, ShellActions& shell, core::DownloadCompleted completed,
                               Callbacks callbacks)
    : Dialog(IDD_COMPLETE, theme), shell_(shell), completed_(std::move(completed)), callbacks_(std::move(callbacks))
{
}

void CompleteDialog::Open(HWND owner)
{
    (void)owner;
    CreateModeless(nullptr);
    if (Handle() != nullptr) {
        ::ShowWindow(Handle(), SW_SHOW);
        ::SetForegroundWindow(Handle());
    }
}

bool CompleteDialog::OnInit()
{
    SetText(IDC_DONE_URL, base::ToUtf16(completed_.url));
    SetText(IDC_DONE_PATH, base::ToUtf16(completed_.finalPath));
    SetText(IDC_DONE_SIZE, completed_.size ? FormatSize(*completed_.size) : L"");
    SHFILEINFOW info{};
    if (::SHGetFileInfoW(base::ToUtf16(completed_.finalPath).c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) != 0) {
        icon_.reset(info.hIcon);
        ::SendMessageW(Item(IDC_DONE_ICON), STM_SETICON, reinterpret_cast<WPARAM>(icon_.get()), 0);
    }
    ::SetWindowSubclass(Item(IDC_DONE_ICON), &CompleteDialog::IconProc, kIconSubclassId, reinterpret_cast<DWORD_PTR>(this));
    ::SetFocus(Item(IDC_DONE_OPEN));
    return false;
}

LRESULT CALLBACK CompleteDialog::IconProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
    if (message == WM_LBUTTONDOWN) {
        POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
        ::ClientToScreen(window, &point);
        if (::DragDetect(window, point)) {
            reinterpret_cast<CompleteDialog*>(data)->StartDrag();  // set by SetWindowSubclass above
            return 0;
        }
    }
    if (message == WM_SETCURSOR) {
        ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    }
    if (message == WM_NCDESTROY) {
        ::RemoveWindowSubclass(window, &CompleteDialog::IconProc, id);
    }
    return ::DefSubclassProc(window, message, wParam, lParam);
}

void CompleteDialog::StartDrag()
{
    // The shell's own data object for the file, so any drop target that accepts Explorer drags
    // (folders, mail, chat programs) receives it.
    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(::SHCreateItemFromParsingName(base::ToUtf16(completed_.finalPath).c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        return;
    }
    Microsoft::WRL::ComPtr<IDataObject> data;
    if (FAILED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data)))) {
        return;
    }
    DWORD effect = DROPEFFECT_NONE;
    ::SHDoDragDrop(Handle(), data.Get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
}

bool CompleteDialog::OnCommand(int id, int code, HWND control)
{
    const std::filesystem::path path(base::ToUtf16(completed_.finalPath));
    switch (id) {
    case IDC_DONE_OPEN:
        if (!shell_.Open(Handle(), path)) {
            Warn(L"The file could not be opened.");
            return true;
        }
        Close(IDOK);
        return true;
    case IDC_DONE_OPEN_WITH:
        shell_.OpenWith(Handle(), path);
        Close(IDOK);
        return true;
    case IDC_DONE_FOLDER:
        shell_.ShowInFolder(path);
        Close(IDOK);
        return true;
    case IDC_DONE_DONT_SHOW:
        return true;
    case IDOK:
    case IDCANCEL:
        Close(id);
        return true;
    default:
        return Dialog::OnCommand(id, code, control);
    }
}

void CompleteDialog::OnDestroying()
{
    if (Checked(IDC_DONE_DONT_SHOW) && callbacks_.dontShowAgain) {
        callbacks_.dontShowAgain();
    }
}

void CompleteDialog::OnDestroyed()
{
    // The callback deletes this object, so it runs from a copy that outlives it.
    const auto closed = callbacks_.closed;
    if (closed) {
        closed(this);
    }
}

CountdownDialog::CountdownDialog(const Theme& theme, std::wstring reason, std::wstring action, int seconds)
    : Dialog(IDD_COUNTDOWN, theme), reason_(std::move(reason)), action_(std::move(action)), remaining_(seconds)
{
}

bool CountdownDialog::Run(HWND owner)
{
    RunModal(owner);
    return confirmed_;
}

bool CountdownDialog::OnInit()
{
    ::SetTimer(Handle(), kCountdownTimer, 1000, nullptr);
    Refresh();
    return true;
}

void CountdownDialog::Refresh()
{
    SetText(IDC_COUNT_TEXT, reason_ + L" Last Download Manager will " + action_ + L" in " + std::to_wstring(remaining_) +
                                L" seconds.");
}

bool CountdownDialog::OnCommand(int id, int code, HWND control)
{
    if (id == IDC_COUNT_NOW) {
        confirmed_ = true;
        ::KillTimer(Handle(), kCountdownTimer);
        Close(IDOK);
        return true;
    }
    if (id == IDCANCEL) {
        ::KillTimer(Handle(), kCountdownTimer);
    }
    return Dialog::OnCommand(id, code, control);
}

bool CountdownDialog::OnMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
{
    (void)lParam;
    if (message != WM_TIMER || wParam != kCountdownTimer) {
        return false;
    }
    remaining_ -= 1;
    if (remaining_ <= 0) {
        confirmed_ = true;
        ::KillTimer(Handle(), kCountdownTimer);
        Close(IDOK);
    } else {
        Refresh();
    }
    result = 0;
    return true;
}

}  // namespace ldm::ui
