#include "ui/dialogs/UpdateDialog.h"

#include "resource.h"

namespace ldm::ui {

UpdateDialog::UpdateDialog(const Theme& theme, std::wstring version)
    : Dialog(IDD_UPDATE, theme), version_(std::move(version))
{
}

UpdateDialog::Choice UpdateDialog::Run(HWND owner)
{
    RunModal(owner);
    return choice_;
}

bool UpdateDialog::OnInit()
{
    SetText(IDC_UPDATE_TEXT, L"Last Download Manager " + version_ +
                                 L" is available. The release page has the installer and what changed.");
    return true;
}

bool UpdateDialog::OnCommand(int id, int code, HWND control)
{
    switch (id) {
    case IDOK:
        choice_ = Choice::OpenPage;
        Close(IDOK);
        return true;
    case IDC_UPDATE_SKIP:
        choice_ = Choice::Skip;
        Close(IDOK);
        return true;
    case IDCANCEL:
        choice_ = Choice::Later;
        Close(IDCANCEL);
        return true;
    default:
        return Dialog::OnCommand(id, code, control);
    }
}

}  // namespace ldm::ui
