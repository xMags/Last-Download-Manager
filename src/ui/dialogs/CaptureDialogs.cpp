#include "ui/dialogs/CaptureDialogs.h"

#include "resource.h"

#include <commctrl.h>

#include <algorithm>
#include <cwctype>

namespace ldm::ui {

namespace {

constexpr size_t kMaxPattern = 2048;

}  // namespace

std::optional<std::wstring> CleanAddressPattern(const std::wstring& text)
{
    const size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return std::nullopt;
    }
    const size_t last = text.find_last_not_of(L" \t\r\n");
    std::wstring pattern = text.substr(first, last - first + 1);
    const bool clean = std::none_of(pattern.begin(), pattern.end(), [](wchar_t c) { return c <= L' ' || c == 0x7F; });
    if (!clean || pattern.size() > kMaxPattern) {
        return std::nullopt;
    }
    return pattern;
}

AddressExceptionsDialog::AddressExceptionsDialog(const Theme& theme, std::vector<std::wstring> addresses)
    : Dialog(IDD_ADDRESS_EXCEPTIONS, theme), addresses_(std::move(addresses))
{
}

std::optional<std::vector<std::wstring>> AddressExceptionsDialog::Run(HWND owner)
{
    RunModal(owner);
    return result_;
}

bool AddressExceptionsDialog::OnInit()
{
    const HWND list = Item(IDC_EXC_LIST);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    if (CurrentTheme().IsDark()) {
        ListView_SetBkColor(list, CurrentTheme().Colors().window);
        ListView_SetTextBkColor(list, CurrentTheme().Colors().window);
        ListView_SetTextColor(list, CurrentTheme().Colors().text);
    }
    RECT client{};
    ::GetClientRect(list, &client);
    LVCOLUMNW column{};
    column.mask = LVCF_WIDTH;
    column.cx = client.right - client.left - ::GetSystemMetricsForDpi(SM_CXVSCROLL, ::GetDpiForWindow(list));
    ListView_InsertColumn(list, 0, &column);
    Refresh(addresses_.empty() ? -1 : 0);
    return true;
}

void AddressExceptionsDialog::Refresh(int select)
{
    const HWND list = Item(IDC_EXC_LIST);
    ListView_DeleteAllItems(list);
    for (size_t i = 0; i < addresses_.size(); ++i) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = addresses_[i].data();  // copied by the list view
        ListView_InsertItem(list, &item);
    }
    if (select >= 0 && select < static_cast<int>(addresses_.size())) {
        ListView_SetItemState(list, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, select, FALSE);
    }
    UpdateButtons();
}

void AddressExceptionsDialog::UpdateButtons()
{
    Enable(IDC_EXC_DELETE, ListView_GetSelectedCount(Item(IDC_EXC_LIST)) > 0);
}

bool AddressExceptionsDialog::OnCommand(int id, int code, HWND control)
{
    switch (id) {
    case IDC_EXC_ADD: {
        AddExceptionDialog add(CurrentTheme());
        if (auto address = add.Run(Handle())) {
            const auto existing = std::find(addresses_.begin(), addresses_.end(), *address);
            if (existing != addresses_.end()) {
                Refresh(static_cast<int>(existing - addresses_.begin()));
            } else {
                addresses_.push_back(std::move(*address));
                Refresh(static_cast<int>(addresses_.size()) - 1);
            }
        }
        return true;
    }
    case IDC_EXC_DELETE: {
        const HWND list = Item(IDC_EXC_LIST);
        int first = -1;
        // Remove from the end so earlier indexes stay valid.
        for (int row = ListView_GetItemCount(list) - 1; row >= 0; --row) {
            if (ListView_GetItemState(list, row, LVIS_SELECTED) != 0) {
                addresses_.erase(addresses_.begin() + row);
                first = row;
            }
        }
        Refresh(std::min(first, static_cast<int>(addresses_.size()) - 1));
        return true;
    }
    case IDOK:
        result_ = addresses_;
        Close(IDOK);
        return true;
    default:
        return Dialog::OnCommand(id, code, control);
    }
}

bool AddressExceptionsDialog::OnNotify(const NMHDR& header, LRESULT& result)
{
    (void)result;
    if (header.idFrom == IDC_EXC_LIST && header.code == LVN_ITEMCHANGED) {
        UpdateButtons();
    }
    if (header.idFrom == IDC_EXC_LIST && header.code == LVN_KEYDOWN &&
        reinterpret_cast<const NMLVKEYDOWN&>(header).wVKey == VK_DELETE) {  // header is the start of NMLVKEYDOWN
        ::SendMessageW(Handle(), WM_COMMAND, MAKEWPARAM(IDC_EXC_DELETE, BN_CLICKED), 0);
    }
    return false;
}

AddExceptionDialog::AddExceptionDialog(const Theme& theme) : Dialog(IDD_ADD_EXCEPTION, theme) {}

std::optional<std::wstring> AddExceptionDialog::Run(HWND owner)
{
    RunModal(owner);
    return address_;
}

bool AddExceptionDialog::OnInit()
{
    ::SetFocus(Item(IDC_EXC_ADDRESS));
    return false;
}

bool AddExceptionDialog::OnCommand(int id, int code, HWND control)
{
    if (id != IDOK) {
        return Dialog::OnCommand(id, code, control);
    }
    auto address = CleanAddressPattern(Text(IDC_EXC_ADDRESS));
    if (!address) {
        Warn(L"Enter an address without spaces, such as https://example.com/files/*.");
        return true;
    }
    address_ = std::move(address);
    Close(IDOK);
    return true;
}

}  // namespace ldm::ui
