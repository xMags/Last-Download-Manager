#include "ui/Dialog.h"

#include <commctrl.h>

#include <cwchar>
#include <iterator>
#include <vector>

namespace ldm::ui {

namespace {

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
        const bool focusDefault = self->OnInit();
        return focusDefault ? TRUE : FALSE;
    }
    auto* self = reinterpret_cast<Dialog*>(::GetWindowLongPtrW(window, DWLP_USER));  // set at WM_INITDIALOG
    if (self == nullptr) {
        return FALSE;
    }
    switch (message) {
    case WM_COMMAND:
        return self->OnCommand(LOWORD(wParam), HIWORD(wParam), reinterpret_cast<HWND>(lParam)) ? TRUE : FALSE;
    case WM_NOTIFY: {
        LRESULT result = 0;
        if (self->OnNotify(*reinterpret_cast<const NMHDR*>(lParam), result)) {
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
        if (!self->theme_.IsDark()) {
            return FALSE;
        }
        const bool editable = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX;
        const auto dc = reinterpret_cast<HDC>(wParam);  // WM_CTLCOLOR* passes the control's DC
        const HBRUSH brush = self->theme_.OnCtlColor(dc, editable);
        if (message == WM_CTLCOLORSTATIC && IsLink(reinterpret_cast<HWND>(lParam))) {
            ::SetTextColor(dc, self->theme_.Colors().accent);  // links stay recognizable as links
        }
        // A dialog procedure returns the brush itself for WM_CTLCOLOR* messages.
        return reinterpret_cast<INT_PTR>(brush);
    }
    case WM_DESTROY:
        if (!self->modal_) {
            self->OnDestroying();
            ::SetWindowLongPtrW(window, DWLP_USER, 0);
            self->window_ = nullptr;
            self->OnDestroyed();  // may delete `self`; nothing below touches it
            return TRUE;
        }
        break;
    default:
        break;
    }
    LRESULT result = 0;
    if (self->OnMessage(message, wParam, lParam, result)) {
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
    ::MessageBoxW(window_, message.c_str(), L"Last Download Manager", MB_OK | MB_ICONWARNING);
}

}  // namespace ldm::ui
