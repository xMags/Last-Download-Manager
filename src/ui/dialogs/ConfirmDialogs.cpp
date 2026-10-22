#include "ui/dialogs/ConfirmDialogs.h"

#include "resource.h"

namespace ldm::ui {

DeleteDialog::DeleteDialog(const Theme& theme, size_t count, bool anyCompleted, bool anyUnfinished)
    : Dialog(IDD_DELETE, theme), count_(count), anyCompleted_(anyCompleted), anyUnfinished_(anyUnfinished)
{
}

std::optional<DeleteDialog::Answer> DeleteDialog::Run(HWND owner)
{
    RunModal(owner);
    return answer_;
}

bool DeleteDialog::OnInit()
{
    std::wstring text = count_ == 1 ? L"Remove the selected download from the list?"
                                    : L"Remove the " + std::to_wstring(count_) + L" selected downloads from the list?";
    if (anyUnfinished_) {
        text += L" Unfinished downloads lose what was downloaded so far.";
    }
    SetText(IDC_DELETE_TEXT, text);
    ::SendMessageW(Item(IDC_DELETE_ICON), STM_SETICON, reinterpret_cast<WPARAM>(::LoadIconW(nullptr, IDI_WARNING)), 0);
    Show(IDC_DELETE_FILES, anyCompleted_);
    return true;
}

bool DeleteDialog::OnCommand(int id, int code, HWND control)
{
    if (id == IDOK) {
        answer_ = Answer{anyCompleted_ && Checked(IDC_DELETE_FILES), Checked(IDC_DELETE_DONT_ASK)};
        Close(IDOK);
        return true;
    }
    return Dialog::OnCommand(id, code, control);
}

DuplicateDialog::DuplicateDialog(const Theme& theme, std::wstring fileName, bool existingComplete)
    : Dialog(IDD_DUPLICATE, theme), fileName_(std::move(fileName)), existingComplete_(existingComplete)
{
}

std::optional<DuplicateDialog::Answer> DuplicateDialog::Run(HWND owner)
{
    RunModal(owner);
    return answer_;
}

bool DuplicateDialog::OnInit()
{
    SetText(IDC_DUP_TEXT, L"This address is already in the list as \"" + fileName_ + L"\"" +
                              (existingComplete_ ? L", which is complete." : L", which is not complete."));
    SetChecked(existingComplete_ ? IDC_DUP_NUMBERED : IDC_DUP_RESUME, true);
    return true;
}

bool DuplicateDialog::OnCommand(int id, int code, HWND control)
{
    if (id == IDOK) {
        Answer answer;
        if (Checked(IDC_DUP_OVERWRITE)) {
            answer.policy = model::DuplicatePolicy::Overwrite;
        } else if (Checked(IDC_DUP_RESUME)) {
            answer.policy = model::DuplicatePolicy::ResumeOrShowComplete;
        }
        answer.remember = Checked(IDC_DUP_REMEMBER);
        answer_ = answer;
        Close(IDOK);
        return true;
    }
    return Dialog::OnCommand(id, code, control);
}

}  // namespace ldm::ui
