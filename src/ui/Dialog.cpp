#include "ui/Dialog.h"

#include "ui/dialogs/MessageDialog.h"

#include <commctrl.h>

#include <cwchar>
#include <iterator>
#include <vector>

namespace ldm::ui {

namespace {

// Private to Dialog's window procedure; dialogs use no other WM_APP messages.
constexpr UINT kBringToFrontMessage = WM_APP + 0x3F0;

bool IsLink(HWND control)
{
    wchar_t className[32] = {};
    ::GetClassNameW(control, className, static_cast<int>(std::size(className)));
    return ::CompareStringOrdinal(className, -1, WC_LINK, -1, TRUE) == CSTR_EQUAL;
}

}  // namespace

Dialog::~Dialog()
{
    if (window_ != nullptr && !modal_ && ::IsWindow(window_)) {
        ::SetWindowLongPtrW(window_, DWLP_USER, 0);
        ::DestroyWindow(window_);
    }
}

void Dialog::BringToFront(HWND window)
{
    // A dialog that opens for something outside LDM (a captured browser download, a second
    // launch, a dropped link) must not hide behind the program the user is in. Windows allows
    // this only when that program passed the right on; otherwise the taskbar button flashes.
    if (::GetForegroundWindow() == window || ::SetForegroundWindow(window)) {
        return;
    }
    // Refused: at least lift the dialog above other windows without taking the keyboard, so it
    // is visible, and flash its taskbar button until the user switches to it.
    constexpr UINT kKeepPlace = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
    ::SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, kKeepPlace);
    ::SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, kKeepPlace);
    FLASHWINFO flash{sizeof(flash), window, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0};
    ::FlashWindowEx(&flash);
}

INT_PTR Dialog::RunModal(HWND owner)
{
    modal_ = true;
    return ::DialogBoxParamW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(templateId_), owner, &Dialog::DialogProc,
                             reinterpret_cast<LPARAM>(this));
}

HWND Dialog::CreateModeless(HWND owner)
{
    modal_ = false;
    return ::CreateDialogParamW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(templateId_), owner, &Dialog::DialogProc,
                                reinterpret_cast<LPARAM>(this));
}

INT_PTR CALLBACK Dialog::DialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_INITDIALOG) {
        auto* self = reinterpret_cast<Dialog*>(lParam);  // passed by RunModal/CreateModeless
        ::SetWindowLongPtrW(window, DWLP_USER, lParam);
        self->window_ = window;
        self->theme_.ApplyToTopLevel(window);
        self->theme_.ApplyToChildren(window);
        if (self->modal_) {
            // Posted: the dialog becomes visible only after WM_INITDIALOG, and Windows ignores
            // a request to bring a hidden window to the front.
            ::PostMessageW(window, kBringToFrontMessage, 0, 0);
        }
        const bool focusDefault = self->OnInit();
        return focusDefault ? TRUE : FALSE;
    }
    auto* self = reinterpret_cast<Dialog*>(::GetWindowLongPtrW(window, DWLP_USER));  // set at WM_INITDIALOG
    if (self == nullptr) {
        return FALSE;
    }
    if (message == WM_DESTROY && self->modal_) {
        // The object outlives a modal dialog (RunModal is still on the stack), so only the
        // cleanup that needs the controls runs here.
        self->OnDestroying();
        return FALSE;
    }
    if (message == WM_DESTROY) {
        self->OnDestroying();
        ::SetWindowLongPtrW(window, DWLP_USER, 0);
        self->window_ = nullptr;
        if (self->dispatchDepth_ > 0) {
            // A handler of this window is below us on the stack and still uses `self`.
            self->destroyPending_ = true;
            return TRUE;
        }
        self->OnDestroyed();  // may delete `self`; nothing below touches it
        return TRUE;
    }
    ++self->dispatchDepth_;
    const INT_PTR result = self->Dispatch(window, message, wParam, lParam);
    if (--self->dispatchDepth_ == 0 && self->destroyPending_) {
        self->destroyPending_ = false;
        self->OnDestroyed();  // may delete `self`; nothing below touches it
    }
    return result;
}

INT_PTR Dialog::Dispatch(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case kBringToFrontMessage:
        BringToFront(window);
        return TRUE;
    case WM_COMMAND:
        return OnCommand(LOWORD(wParam), HIWORD(wParam), reinterpret_cast<HWND>(lParam)) ? TRUE : FALSE;
    case WM_NOTIFY: {
        LRESULT result = 0;
        if (OnNotify(*reinterpret_cast<const NMHDR*>(lParam), result)) {
            ::SetWindowLongPtrW(window, DWLP_MSGRESULT, result);
            return TRUE;
        }
        return FALSE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        if (!theme_.IsDark()) {
            return FALSE;
        }
        const bool editable = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX;
        const auto dc = reinterpret_cast<HDC>(wParam);  // WM_CTLCOLOR* passes the control's DC
        const HBRUSH brush = theme_.OnCtlColor(dc, editable);
        if (message == WM_CTLCOLORSTATIC && IsLink(reinterpret_cast<HWND>(lParam))) {
            ::SetTextColor(dc, theme_.Colors().accent);  // links stay recognizable as links
        }
        // A dialog procedure returns the brush itself for WM_CTLCOLOR* messages.
        return reinterpret_cast<INT_PTR>(brush);
    }
    default:
        break;
    }
    LRESULT result = 0;
    if (OnMessage(message, wParam, lParam, result)) {
        ::SetWindowLongPtrW(window, DWLP_MSGRESULT, result);
        return TRUE;
    }
    return FALSE;
}

bool Dialog::OnCommand(int id, int code, HWND control)
{
    (void)code;
    (void)control;
    if (id == IDOK || id == IDCANCEL) {
        Close(id);
        return true;
    }
    return false;
}

void Dialog::Close(INT_PTR result)
{
    if (modal_) {
        ::EndDialog(window_, result);
    } else {
        ::DestroyWindow(window_);
    }
}

HWND Dialog::Item(int id) const
{
    return ::GetDlgItem(window_, id);
}

std::wstring Dialog::Text(int id) const
{
    const HWND control = Item(id);
    const int length = ::GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied = ::GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(copied));
    return text;
}

void Dialog::SetText(int id, const std::wstring& text)
{
    ::SetWindowTextW(Item(id), text.c_str());
}

bool Dialog::Checked(int id) const
{
    return ::IsDlgButtonChecked(window_, id) == BST_CHECKED;
}

void Dialog::SetChecked(int id, bool checked)
{
    ::CheckDlgButton(window_, id, checked ? BST_CHECKED : BST_UNCHECKED);
}

void Dialog::Enable(int id, bool enabled)
{
    ::EnableWindow(Item(id), enabled ? TRUE : FALSE);
}

void Dialog::Show(int id, bool visible)
{
    ::ShowWindow(Item(id), visible ? SW_SHOW : SW_HIDE);
}

std::optional<int> Dialog::Number(int id) const
{
    const std::wstring text = Text(id);
    if (text.empty()) {
        return std::nullopt;
    }
    wchar_t* end = nullptr;
    const long value = std::wcstol(text.c_str(), &end, 10);
    if (end == nullptr || *end != L'\0' || value < INT_MIN || value > INT_MAX) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

void Dialog::SetNumber(int id, std::int64_t value)
{
    SetText(id, std::to_wstring(value));
}

void Dialog::Warn(const std::wstring& message) const
{
    (void)ShowMessage(window_, theme_, MessageRequest{message, MessageKind::Warning});
}

}  // namespace ldm::ui
