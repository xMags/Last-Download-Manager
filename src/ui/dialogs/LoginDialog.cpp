#include "ui/dialogs/LoginDialog.h"

#include "base/Utf.h"
#include "resource.h"

namespace ldm::ui {

LoginDialog::LoginDialog(const Theme& theme, std::wstring server, std::wstring fileName)
    : Dialog(IDD_LOGIN, theme), server_(std::move(server)), fileName_(std::move(fileName))
{
}

std::optional<LoginDialog::Answer> LoginDialog::Run(HWND owner)
{
    RunModal(owner);
    return answer_;
}

bool LoginDialog::OnInit()
{
    SetText(IDC_LOGIN_TEXT, L"The server " + server_ + L" requires a login to download \"" + fileName_ + L"\".");
    ::SetFocus(Item(IDC_LOGIN_USER));
    return false;
}

bool LoginDialog::OnCommand(int id, int code, HWND control)
{
    if (id == IDOK) {
        Answer answer{base::ToUtf8(Text(IDC_LOGIN_USER)), base::ToUtf8(Text(IDC_LOGIN_PASSWORD)), Checked(IDC_LOGIN_SAVE)};
        if (answer.user.empty()) {
            Warn(L"Enter the login.");
            return true;
        }
        answer_ = std::move(answer);
        Close(IDOK);
        return true;
    }
    return Dialog::OnCommand(id, code, control);
}

}  // namespace ldm::ui
