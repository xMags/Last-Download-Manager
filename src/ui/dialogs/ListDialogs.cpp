#include "ui/dialogs/ListDialogs.h"

#include "base/Utf.h"
#include "resource.h"

#include <commctrl.h>

#include <algorithm>

namespace ldm::ui {

namespace {

void AddSingleColumn(HWND list)
{
    RECT client{};
    ::GetClientRect(list, &client);
    LVCOLUMNW column{};
    column.mask = LVCF_WIDTH;
    column.cx = client.right - ::GetSystemMetricsForDpi(SM_CXVSCROLL, ::GetDpiForWindow(list));
    ListView_InsertColumn(list, 0, &column);
}

void ApplyListTheme(HWND list, const Theme& theme)
{
    if (theme.IsDark()) {
        ListView_SetBkColor(list, theme.Colors().window);
        ListView_SetTextBkColor(list, theme.Colors().window);
        ListView_SetTextColor(list, theme.Colors().text);
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Find

FindDialog::FindDialog(const Theme& theme, FindOptions initial, std::function<bool(const FindOptions&)> findNext,
                       std::function<void()> closed)
    : Dialog(IDD_FIND, theme), options_(std::move(initial)), findNext_(std::move(findNext)), closed_(std::move(closed))
{
}

void FindDialog::Open(HWND owner)
{
    CreateModeless(owner);
    if (Handle() != nullptr) {
        ::ShowWindow(Handle(), SW_SHOW);
    }
}

bool FindDialog::OnInit()
{
    SetText(IDC_FIND_TEXT, options_.text);
    SetChecked(IDC_FIND_NAME, options_.inFileName);
    SetChecked(IDC_FIND_DESCRIPTION, options_.inDescription);
    SetChecked(IDC_FIND_ADDRESSES, options_.inAddresses);
    SetChecked(IDC_FIND_CASE, options_.matchCase);
    SetChecked(IDC_FIND_WHOLE, options_.wholeString);
    ::SetFocus(Item(IDC_FIND_TEXT));
    ::SendMessageW(Item(IDC_FIND_TEXT), EM_SETSEL, 0, -1);
    return false;
}

void FindDialog::ReadOptions()
{
    options_.text = Text(IDC_FIND_TEXT);
    options_.inFileName = Checked(IDC_FIND_NAME);
    options_.inDescription = Checked(IDC_FIND_DESCRIPTION);
    options_.inAddresses = Checked(IDC_FIND_ADDRESSES);
    options_.matchCase = Checked(IDC_FIND_CASE);
    options_.wholeString = Checked(IDC_FIND_WHOLE);
}

bool FindDialog::OnCommand(int id, int code, HWND control)
{
    if (id == IDC_FIND_NEXT || id == IDOK) {
        ReadOptions();
        if (options_.text.empty()) {
            return true;
        }
        if (!findNext_(options_)) {
            ::MessageBeep(MB_ICONASTERISK);
            Warn(L"No download matches \"" + options_.text + L"\".");
        }
        return true;
    }
    if (id == IDCANCEL) {
        ReadOptions();
        Close(IDCANCEL);
        return true;
    }
    return Dialog::OnCommand(id, code, control);
}

void FindDialog::OnDestroyed()
{
    // The callback may delete this object, so it runs from a copy that outlives it.
    const auto closed = closed_;
    if (closed) {
        closed();
    }
}

// ---------------------------------------------------------------------------------------------
// Columns

ColumnsDialog::ColumnsDialog(const Theme& theme, std::vector<ColumnState> columns)
    : Dialog(IDD_COLUMNS, theme), columns_(std::move(columns))
{
}

std::optional<std::vector<ColumnState>> ColumnsDialog::Run(HWND owner)
{
    RunModal(owner);
    return result_;
}

bool ColumnsDialog::OnInit()
{
    const HWND list = Item(IDC_COL_LIST);
    ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ApplyListTheme(list, CurrentTheme());
    AddSingleColumn(list);
    Fill();
    return true;
}

void ColumnsDialog::Fill()
{
    filling_ = true;
    const HWND list = Item(IDC_COL_LIST);
    const int selected = Selected();
    ListView_DeleteAllItems(list);
    for (size_t i = 0; i < columns_.size(); ++i) {
        std::wstring title(ColumnTitle(columns_[i].id));
        if (columns_[i].id == Column::Queue) {
            title = L"Q (queue)";
        }
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = title.data();
        ListView_InsertItem(list, &item);
        ListView_SetCheckState(list, static_cast<int>(i), columns_[i].visible);
    }
    const int focus = std::clamp(selected, 0, static_cast<int>(columns_.size()) - 1);
    ListView_SetItemState(list, focus, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    SetNumber(IDC_COL_WIDTH, columns_[static_cast<size_t>(focus)].width);
    filling_ = false;
}

int ColumnsDialog::Selected() const
{
    return ListView_GetNextItem(Item(IDC_COL_LIST), -1, LVNI_SELECTED);
}

void ColumnsDialog::ReadChecks()
{
    const HWND list = Item(IDC_COL_LIST);
    for (size_t i = 0; i < columns_.size(); ++i) {
        columns_[i].visible = ListView_GetCheckState(list, static_cast<int>(i)) != FALSE;
    }
}

void ColumnsDialog::Move(int delta)
{
    const int selected = Selected();
    const int target = selected + delta;
    if (selected < 0 || target < 0 || target >= static_cast<int>(columns_.size())) {
        return;
    }
    ReadChecks();
    std::swap(columns_[static_cast<size_t>(selected)], columns_[static_cast<size_t>(target)]);
    Fill();
    ListView_SetItemState(Item(IDC_COL_LIST), -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(Item(IDC_COL_LIST), target, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

bool ColumnsDialog::OnNotify(const NMHDR& header, LRESULT& result)
{
    if (header.idFrom == IDC_COL_LIST && header.code == LVN_ITEMCHANGED && !filling_) {
        const auto& change = reinterpret_cast<const NMLISTVIEW&>(header);
        if ((change.uNewState & LVIS_SELECTED) != 0 && change.iItem >= 0) {
            filling_ = true;
            SetNumber(IDC_COL_WIDTH, columns_[static_cast<size_t>(change.iItem)].width);
            filling_ = false;
        }
        result = 0;
        return true;
    }
    return false;
}

bool ColumnsDialog::OnCommand(int id, int code, HWND control)
{
    switch (id) {
    case IDC_COL_UP:
        Move(-1);
        return true;
    case IDC_COL_DOWN:
        Move(1);
        return true;
    case IDC_COL_WIDTH:
        if (code == EN_CHANGE && !filling_) {
            const int selected = Selected();
            const auto width = Number(IDC_COL_WIDTH);
            if (selected >= 0 && width && *width >= 16 && *width <= 2000) {
                columns_[static_cast<size_t>(selected)].width = *width;
            }
        }
        return true;
    case IDC_COL_RESET:
        columns_ = DefaultColumns();
        Fill();
        return true;
    case IDOK:
        ReadChecks();
        if (std::none_of(columns_.begin(), columns_.end(), [](const ColumnState& c) { return c.visible; })) {
            Warn(L"Show at least one column.");
            return true;
        }
        result_ = columns_;
        Close(IDOK);
        return true;
    default:
        return Dialog::OnCommand(id, code, control);
    }
}

// ---------------------------------------------------------------------------------------------
// Address list

UrlListDialog::UrlListDialog(const Theme& theme, std::wstring title, std::vector<std::string> urls)
    : Dialog(IDD_URL_LIST, theme), title_(std::move(title)), urls_(std::move(urls))
{
}

std::optional<UrlListDialog::Choice> UrlListDialog::Run(HWND owner)
{
    RunModal(owner);
    return result_;
}

bool UrlListDialog::OnInit()
{
    SetTitle(title_);
    const HWND list = Item(IDC_LIST_URLS);
    ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ApplyListTheme(list, CurrentTheme());
    AddSingleColumn(list);
    for (size_t i = 0; i < urls_.size(); ++i) {
        std::wstring text = base::ToUtf16(urls_[i]);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = text.data();
        ListView_InsertItem(list, &item);
        ListView_SetCheckState(list, static_cast<int>(i), TRUE);
    }
    SetText(IDC_LIST_COUNT, std::to_wstring(urls_.size()) + (urls_.size() == 1 ? L" address found." : L" addresses found."));
    return true;
}

bool UrlListDialog::OnCommand(int id, int code, HWND control)
{
    const HWND list = Item(IDC_LIST_URLS);
    switch (id) {
    case IDC_LIST_ALL:
    case IDC_LIST_NONE:
        for (size_t i = 0; i < urls_.size(); ++i) {
            ListView_SetCheckState(list, static_cast<int>(i), id == IDC_LIST_ALL);
        }
        return true;
    case IDOK: {
        Choice choice;
        for (size_t i = 0; i < urls_.size(); ++i) {
            if (ListView_GetCheckState(list, static_cast<int>(i))) {
                choice.urls.push_back(urls_[i]);
            }
        }
        choice.start = Checked(IDC_LIST_START);
        result_ = std::move(choice);
        Close(IDOK);
        return true;
    }
    default:
        return Dialog::OnCommand(id, code, control);
    }
}

}  // namespace ldm::ui
