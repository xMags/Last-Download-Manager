#include "ui/dialogs/BatchDialog.h"

#include "base/Utf.h"
#include "core/Url.h"
#include "resource.h"

namespace ldm::ui {

BatchDialog::BatchDialog(const Theme& theme) : Dialog(IDD_BATCH, theme) {}

std::optional<BatchDialog::Choice> BatchDialog::Run(HWND owner)
{
    RunModal(owner);
    return result_;
}

bool BatchDialog::OnInit()
{
    SetChecked(IDC_BATCH_NUMBERS, true);
    SetNumber(IDC_BATCH_FROM, 1);
    SetNumber(IDC_BATCH_TO, 10);
    SetNumber(IDC_BATCH_WIDTH, 1);
    SetText(IDC_BATCH_FROM_LETTER, L"a");
    SetText(IDC_BATCH_TO_LETTER, L"z");
    ::SendMessageW(Item(IDC_BATCH_FROM_LETTER), EM_LIMITTEXT, 1, 0);
    ::SendMessageW(Item(IDC_BATCH_TO_LETTER), EM_LIMITTEXT, 1, 0);
    initializing_ = false;
    UpdateEnabled();
    UpdatePreview();
    ::SetFocus(Item(IDC_BATCH_URL));
    return false;
}

BatchPattern BatchDialog::ReadPattern() const
{
    BatchPattern pattern;
    pattern.address = base::ToUtf8(Text(IDC_BATCH_URL));
    pattern.letters = Checked(IDC_BATCH_LETTERS);
    pattern.fromNumber = Number(IDC_BATCH_FROM).value_or(-1);
    pattern.toNumber = Number(IDC_BATCH_TO).value_or(-1);
    pattern.width = Number(IDC_BATCH_WIDTH).value_or(0);
    const std::wstring from = Text(IDC_BATCH_FROM_LETTER);
    const std::wstring to = Text(IDC_BATCH_TO_LETTER);
    pattern.fromLetter = from.size() == 1 && from[0] < 128 ? static_cast<char>(from[0]) : '\0';
    pattern.toLetter = to.size() == 1 && to[0] < 128 ? static_cast<char>(to[0]) : '\0';
    return pattern;
}

void BatchDialog::UpdateEnabled()
{
    const bool letters = Checked(IDC_BATCH_LETTERS);
    for (const int id : {IDC_BATCH_FROM, IDC_BATCH_TO, IDC_BATCH_WIDTH}) {
        Enable(id, !letters);
    }
    for (const int id : {IDC_BATCH_FROM_LETTER, IDC_BATCH_TO_LETTER}) {
        Enable(id, letters);
    }
    const bool auth = Checked(IDC_BATCH_AUTH);
    Enable(IDC_BATCH_USER, auth);
    Enable(IDC_BATCH_PASSWORD, auth);
}

void BatchDialog::UpdatePreview()
{
    const BatchExpansion expansion = ExpandBatch(ReadPattern());
    if (expansion.error || expansion.addresses.empty()) {
        SetText(IDC_BATCH_FIRST, base::ToUtf16(expansion.error.value_or("")));
        SetText(IDC_BATCH_SECOND, L"");
        SetText(IDC_BATCH_LAST, L"");
        return;
    }
    const auto& addresses = expansion.addresses;
    SetText(IDC_BATCH_FIRST, base::ToUtf16(addresses.front()));
    SetText(IDC_BATCH_SECOND, addresses.size() > 2 ? base::ToUtf16(addresses[1]) : L"");
    SetText(IDC_BATCH_LAST, addresses.size() > 1 ? (addresses.size() > 2 ? L"... " : L"") + base::ToUtf16(addresses.back()) : L"");
}

bool BatchDialog::OnCommand(int id, int code, HWND control)
{
    if (initializing_) {
        return false;
    }
    switch (id) {
    case IDC_BATCH_NUMBERS:
    case IDC_BATCH_LETTERS:
    case IDC_BATCH_AUTH:
        UpdateEnabled();
        UpdatePreview();
        return true;
    case IDC_BATCH_URL:
    case IDC_BATCH_FROM:
    case IDC_BATCH_TO:
    case IDC_BATCH_WIDTH:
    case IDC_BATCH_FROM_LETTER:
    case IDC_BATCH_TO_LETTER:
        if (code == EN_CHANGE) {
            UpdatePreview();
        }
        return true;
    case IDOK: {
        BatchExpansion expansion = ExpandBatch(ReadPattern());
        if (expansion.error) {
            Warn(base::ToUtf16(*expansion.error));
            return true;
        }
        if (expansion.addresses.empty() || !core::ParseUrl(expansion.addresses.front())) {
            Warn(L"The address is not a valid http, https or ftp address.");
            return true;
        }
        Choice choice;
        choice.urls = std::move(expansion.addresses);
        if (Checked(IDC_BATCH_AUTH)) {
            const std::string user = base::ToUtf8(Text(IDC_BATCH_USER));
            if (!user.empty()) {
                choice.user = user;
                choice.password = base::ToUtf8(Text(IDC_BATCH_PASSWORD));
            }
        }
        choice.start = Checked(IDC_BATCH_START);
        result_ = std::move(choice);
        Close(IDOK);
        return true;
    }
    default:
        return Dialog::OnCommand(id, code, control);
    }
}

}  // namespace ldm::ui
