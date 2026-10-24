#include "ui/dialogs/QueueDialogs.h"

#include "base/Utf.h"
#include "resource.h"

#include <windowsx.h>

#include <algorithm>

namespace ldm::ui {

namespace {

// The core's limit is 100 bytes of UTF-8; this keeps most names within it and the core reports
// the rest.
constexpr int kMaxNameCharacters = 100;

std::wstring Trimmed(const std::wstring& text)
{
    const auto first = text.find_first_not_of(L" \t");
    if (first == std::wstring::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(L" \t") - first + 1);
}

}  // namespace

QueueNameDialog::QueueNameDialog(const Theme& theme, std::vector<std::wstring> takenNames)
    : Dialog(IDD_QUEUE_NAME, theme), taken_(std::move(takenNames))
{
}

std::optional<std::string> QueueNameDialog::Run(HWND owner)
{
    RunModal(owner);
    return name_;
}

bool QueueNameDialog::OnInit()
{
    ::SendMessageW(Item(IDC_QUEUE_NAME), EM_LIMITTEXT, kMaxNameCharacters, 0);
    ::SetFocus(Item(IDC_QUEUE_NAME));
    return false;
}

bool QueueNameDialog::OnCommand(int id, int code, HWND control)
{
    if (id != IDOK) {
        return Dialog::OnCommand(id, code, control);
    }
    const std::wstring name = Trimmed(Text(IDC_QUEUE_NAME));
    if (name.empty()) {
        Warn(L"Enter a name for the queue.");
        return true;
    }
    const bool taken = std::any_of(taken_.begin(), taken_.end(), [&name](const std::wstring& other) {
        return ::CompareStringOrdinal(other.c_str(), static_cast<int>(other.size()), name.c_str(),
                                      static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
    });
    if (taken) {
        Warn(L"A queue with this name already exists.");
        return true;
    }
    name_ = base::ToUtf8(name);
    Close(IDOK);
    return true;
}

QueuePanelDialog::QueuePanelDialog(const Theme& theme, std::vector<Entry> queues, int downloads)
    : Dialog(IDD_QUEUE_PANEL, theme), queues_(std::move(queues)), downloads_(downloads)
{
}

std::optional<QueuePanelDialog::Choice> QueuePanelDialog::Run(HWND owner)
{
    RunModal(owner);
    return choice_;
}

bool QueuePanelDialog::OnInit()
{
    SetText(IDC_QPANEL_TEXT, downloads_ == 1 ? std::wstring(L"Choose the queue for the download.")
                                             : L"Choose the queue for the " + std::to_wstring(downloads_) + L" downloads.");
    const HWND combo = Item(IDC_QPANEL_QUEUE);
    for (const Entry& entry : queues_) {
        ComboBox_AddString(combo, entry.name.c_str());
    }
    ComboBox_SetCurSel(combo, 0);
    ::SetFocus(combo);
    return false;
}

bool QueuePanelDialog::OnCommand(int id, int code, HWND control)
{
    if (id != IDOK) {
        return Dialog::OnCommand(id, code, control);
    }
    const int selected = ComboBox_GetCurSel(Item(IDC_QPANEL_QUEUE));
    if (selected < 0 || static_cast<size_t>(selected) >= queues_.size()) {
        Close(IDCANCEL);
        return true;
    }
    choice_ = Choice{queues_[static_cast<size_t>(selected)].id, Checked(IDC_QPANEL_START), Checked(IDC_QPANEL_DONT_ASK)};
    Close(IDOK);
    return true;
}

}  // namespace ldm::ui
