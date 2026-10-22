#include "ui/dialogs/AddUrlDialog.h"

#include "base/Utf.h"
#include "core/Url.h"
#include "resource.h"

#include <windowsx.h>

namespace ldm::ui {

AddUrlDialog::AddUrlDialog(const Theme& theme, std::vector<std::string> history, std::string initialUrl)
    : Dialog(IDD_ADD_URL, theme), history_(std::move(history)), initialUrl_(std::move(initialUrl))
{
}

std::optional<AddUrlInput> AddUrlDialog::Run(HWND owner)
{
    RunModal(owner);
    return result_;
}

bool AddUrlDialog::OnInit()
{
    const HWND combo = Item(IDC_ADD_URL);
    for (const std::string& entry : history_) {
        ComboBox_AddString(combo, base::ToUtf16(entry).c_str());
    }
    SetText(IDC_ADD_URL, base::ToUtf16(initialUrl_));
    ComboBox_SetEditSel(combo, 0, -1);
    UpdateAuthControls();
    ::SetFocus(combo);
    return false;  // focus set explicitly
}

void AddUrlDialog::UpdateAuthControls()
{
    const bool enabled = Checked(IDC_ADD_AUTH);
    for (const int id : {IDC_ADD_USER, IDC_ADD_PASSWORD, IDC_ADD_USER_LABEL, IDC_ADD_PASSWORD_LABEL}) {
        Enable(id, enabled);
    }
}

bool AddUrlDialog::OnCommand(int id, int code, HWND control)
{
    if (id == IDC_ADD_AUTH && code == BN_CLICKED) {
        UpdateAuthControls();
        return true;
    }
    if (id == IDOK) {
        const std::string url = base::ToUtf8(Text(IDC_ADD_URL));
        if (!core::ParseUrl(url)) {
            Warn(L"Enter a valid address that starts with http://, https://, ftp:// or ftps://.");
            ::SetFocus(Item(IDC_ADD_URL));
            return true;
        }
        AddUrlInput input;
        input.url = url;
        if (Checked(IDC_ADD_AUTH)) {
            const std::string user = base::ToUtf8(Text(IDC_ADD_USER));
            if (!user.empty()) {
                input.user = user;
                input.password = base::ToUtf8(Text(IDC_ADD_PASSWORD));
            }
        }
        result_ = std::move(input);
        Close(IDOK);
        return true;
    }
    return Dialog::OnCommand(id, code, control);
}

}  // namespace ldm::ui
