#include "ui/dialogs/MessageDialog.h"

#include "resource.h"
#include "ui/Dialog.h"
#include "ui/Dpi.h"
#include "ui/Handles.h"

#include <commctrl.h>

#include <algorithm>

namespace ldm::ui {

namespace {

constexpr wchar_t kTitle[] = L"Last Download Manager";

UINT IconFlag(MessageKind kind)
{
    switch (kind) {
    case MessageKind::Warning:
        return MB_ICONWARNING;
    case MessageKind::Question:
        return MB_ICONQUESTION;
    case MessageKind::Information:
        break;
    }
    return MB_ICONINFORMATION;
}

const wchar_t* IconId(MessageKind kind)
{
    switch (kind) {
    case MessageKind::Warning:
        return IDI_WARNING;
    case MessageKind::Question:
        return IDI_QUESTION;
    case MessageKind::Information:
        break;
    }
    return IDI_INFORMATION;
}

class MessageDialog final : public Dialog {
public:
    MessageDialog(const Theme& theme, const MessageRequest& request) : Dialog(IDD_MESSAGE, theme), request_(request) {}

private:
    bool OnInit() override
    {
        const bool yesNo = request_.buttons != MessageButtons::Ok;
        const bool cancel = request_.buttons == MessageButtons::YesNoCancel;
        SetText(IDC_MESSAGE_TEXT, request_.text);
        SetText(IDOK, yesNo ? L"&Yes" : L"OK");
        Show(IDNO, yesNo);
        Enable(IDNO, yesNo);
        Show(IDCANCEL, cancel);
        Enable(IDCANCEL, cancel);
        if (yesNo && !cancel) {
            // A question needs an answer: no close button and no Esc, as in the Windows message box.
            ::EnableMenuItem(::GetSystemMenu(Handle(), FALSE), SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
        }
        const UINT dpi = DpiOf(Handle());
        const int iconSize = ::GetSystemMetricsForDpi(SM_CXICON, dpi);
        HICON icon = nullptr;
        if (SUCCEEDED(::LoadIconWithScaleDown(nullptr, IconId(request_.kind), iconSize, iconSize, &icon))) {
            icon_.reset(icon);
            ::SendMessageW(Item(IDC_MESSAGE_ICON), STM_SETICON, reinterpret_cast<WPARAM>(icon), 0);  // STM_SETICON takes the HICON
        }
        Layout(dpi, iconSize);
        ::MessageBeep(IconFlag(request_.kind));
        if (request_.bringToFront) {
            ::SetForegroundWindow(Handle());
        }
        return true;  // focus the first button
    }

    bool OnCommand(int id, int code, HWND control) override
    {
        (void)code;
        (void)control;
        const bool yesNo = request_.buttons != MessageButtons::Ok;
        switch (id) {
        case IDOK:
            Close(yesNo ? IDYES : IDOK);
            return true;
        case IDNO:
            Close(IDNO);
            return true;
        case IDCANCEL:
            // The Cancel button, Esc or the close button.
            if (request_.buttons == MessageButtons::YesNoCancel) {
                Close(IDCANCEL);
            } else if (!yesNo) {
                Close(IDOK);
            }
            return true;
        default:
            return false;
        }
    }

    // Sizes the dialog to the text, as the Windows message box does: icon on the left, text
    // wrapped at a readable width, buttons at the bottom right, centered over the owner.
    void Layout(UINT dpi, int iconSize)
    {
        const HWND window = Handle();
        const HWND text = Item(IDC_MESSAGE_TEXT);
        const int margin = Scale(14, dpi);
        const int gap = Scale(12, dpi);
        const int buttonGap = Scale(6, dpi);
        RECT button{};
        ::GetWindowRect(Item(IDOK), &button);  // the template's size, which follows the dialog font
        const int buttonWidth = button.right - button.left;
        const int buttonHeight = button.bottom - button.top;
        const int buttonCount = request_.buttons == MessageButtons::YesNoCancel ? 3
                                : request_.buttons == MessageButtons::YesNo     ? 2
                                                                                : 1;
        const int buttonsWidth = buttonCount * buttonWidth + (buttonCount - 1) * buttonGap;

        RECT measured{0, 0, Scale(400, dpi), 0};
        {
            const HDC dc = ::GetDC(text);
            {
                SelectObjectScope font(dc, reinterpret_cast<HGDIOBJ>(::SendMessageW(window, WM_GETFONT, 0, 0)));  // returns the HFONT
                ::DrawTextW(dc, request_.text.c_str(), static_cast<int>(request_.text.size()), &measured,
                            DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX | DT_EXPANDTABS);
            }
            ::ReleaseDC(text, dc);
        }
        const int textWidth = measured.right - measured.left;
        const int textHeight = measured.bottom - measured.top;
        const int bodyHeight = std::max(iconSize, textHeight);
        const int clientWidth = 2 * margin + std::max({iconSize + gap + textWidth, buttonsWidth, Scale(240, dpi)});
        const int buttonsTop = margin + bodyHeight + Scale(18, dpi);
        const int clientHeight = buttonsTop + buttonHeight + Scale(12, dpi);

        ::MoveWindow(Item(IDC_MESSAGE_ICON), margin, margin + (bodyHeight - iconSize) / 2, iconSize, iconSize, FALSE);
        ::MoveWindow(text, margin + iconSize + gap, margin + (bodyHeight - textHeight) / 2, textWidth, textHeight, FALSE);
        const int buttonsLeft = clientWidth - margin - buttonsWidth;
        ::MoveWindow(Item(IDOK), buttonsLeft, buttonsTop, buttonWidth, buttonHeight, FALSE);
        ::MoveWindow(Item(IDNO), buttonsLeft + buttonWidth + buttonGap, buttonsTop, buttonWidth, buttonHeight, FALSE);
        ::MoveWindow(Item(IDCANCEL), buttonsLeft + 2 * (buttonWidth + buttonGap), buttonsTop, buttonWidth, buttonHeight,
                     FALSE);

        RECT frame{0, 0, clientWidth, clientHeight};
        ::AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(::GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
                                   static_cast<DWORD>(::GetWindowLongPtrW(window, GWL_EXSTYLE)), dpi);
        const int width = frame.right - frame.left;
        const int height = frame.bottom - frame.top;
        MONITORINFO monitor{sizeof(monitor)};
        ::GetMonitorInfoW(::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        RECT anchor = monitor.rcWork;
        const HWND owner = ::GetWindow(window, GW_OWNER);
        if (owner != nullptr && ::IsWindowVisible(owner) != FALSE && ::IsIconic(owner) == FALSE) {
            ::GetWindowRect(owner, &anchor);
        }
        const RECT& work = monitor.rcWork;
        const int x = std::clamp((anchor.left + anchor.right - width) / 2, work.left, std::max(work.left, work.right - width));
        const int y = std::clamp((anchor.top + anchor.bottom - height) / 2, work.top, std::max(work.top, work.bottom - height));
        ::SetWindowPos(window, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    const MessageRequest& request_;
    UniqueIcon icon_;
};

}  // namespace

int ShowMessage(HWND owner, const Theme& theme, const MessageRequest& request)
{
    if (theme.IsDark()) {
        MessageDialog dialog(theme, request);
        const INT_PTR result = dialog.RunModal(owner);
        if (result != -1) {
            return static_cast<int>(result);
        }
        // The dialog could not be created; the Windows message box still gets the answer.
    }
    UINT flags = IconFlag(request.kind);
    switch (request.buttons) {
    case MessageButtons::Ok:
        flags |= MB_OK;
        break;
    case MessageButtons::YesNo:
        flags |= MB_YESNO;
        break;
    case MessageButtons::YesNoCancel:
        flags |= MB_YESNOCANCEL;
        break;
    }
    if (request.bringToFront) {
        flags |= MB_SETFOREGROUND;
    }
    return ::MessageBoxW(owner, request.text.c_str(), kTitle, flags);
}

}  // namespace ldm::ui
