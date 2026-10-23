#include "ui/dialogs/AllLinksDialog.h"

#include "base/Utf.h"
#include "core/FileNames.h"
#include "resource.h"
#include "ui/FileDialogs.h"
#include "ui/Format.h"
#include "ui/Handles.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <set>

namespace ldm::ui {

namespace {

enum LinkColumn {
    kNameColumn,
    kTypeColumn,
    kSizeColumn,
    kTextColumn,
    kAddressColumn,
    kColumnCount,
};

// Share of the list's width per column.
constexpr std::array<int, kColumnCount> kColumnPercent = {24, 7, 10, 22, 37};
constexpr std::array<const wchar_t*, kColumnCount> kColumnTitles = {L"File name", L"Type", L"Size", L"Link text",
                                                                   L"Address"};

std::wstring SizeText(const LinkRow& row)
{
    if (row.check == LinkCheck::Pending) {
        return {};
    }
    return row.size ? FormatSize(*row.size) : L"Unknown";
}

// Orders rows for a column; ties keep the page's order.
bool RowLess(const LinkRow& a, const LinkRow& b, int column)
{
    switch (column) {
    case kNameColumn:
        return ::CompareStringOrdinal(base::ToUtf16(a.fileName).c_str(), -1, base::ToUtf16(b.fileName).c_str(), -1,
                                      TRUE) == CSTR_LESS_THAN;
    case kTypeColumn:
        return LinkTypeText(a) < LinkTypeText(b);
    case kSizeColumn:
        return a.size.value_or(-1) < b.size.value_or(-1);
    case kTextColumn:
        return ::CompareStringOrdinal(base::ToUtf16(a.link.text).c_str(), -1, base::ToUtf16(b.link.text).c_str(), -1,
                                      TRUE) == CSTR_LESS_THAN;
    default:
        return a.link.url < b.link.url;
    }
}

}  // namespace

AllLinksDialog::AllLinksDialog(const Theme& theme, Initial initial)
    : Dialog(IDD_ALL_LINKS, theme), initial_(std::move(initial)), rows_(initial_.rows)
{
}

std::optional<AllLinksDialog::Choice> AllLinksDialog::Run(HWND owner)
{
    RunModal(owner);
    return result_;
}

bool AllLinksDialog::OnInit()
{
    if (initial_.selection) {
        ::SetWindowTextW(Handle(), L"Download Selected Links with LDM");
    }
    const HWND list = Item(IDC_LINKS_LIST);
    ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                                LVS_EX_LABELTIP | LVS_EX_HEADERDRAGDROP);
    if (CurrentTheme().IsDark()) {
        ListView_SetBkColor(list, CurrentTheme().Colors().window);
        ListView_SetTextBkColor(list, CurrentTheme().Colors().window);
        ListView_SetTextColor(list, CurrentTheme().Colors().text);
    }
    AddColumns();

    const HWND combo = Item(IDC_LINKS_CATEGORY);
    int selected = 0;
    for (const model::Category& category : initial_.categories) {
        const int index = ComboBox_AddString(combo, base::ToUtf16(category.name).c_str());
        ComboBox_SetItemData(combo, index, category.id.value);
        if (initial_.category == category.id) {
            selected = index;
        }
    }
    ComboBox_SetCurSel(combo, selected);
    SetText(IDC_LINKS_FOLDER, base::ToUtf16(initial_.folder));
    SetChecked(IDC_LINKS_BY_CATEGORY, true);
    SetChecked(IDC_LINKS_USE_TEXT, initial_.useLinkText);
    SetChecked(IDC_LINKS_HIDE_HTML, initial_.filter.hideHtml);
    SetChecked(IDC_LINKS_HIDE_IMAGES, initial_.filter.hideImages);
    SetChecked(IDC_LINKS_HIDE_REPEATED, initial_.filter.hideRepeated);
    UpdateSaveTo();
    Refill();
    if (ListView_GetItemCount(list) > 0) {
        ListView_SetItemState(list, 0, LVIS_FOCUSED, LVIS_FOCUSED);
    }
    ::SetFocus(list);
    return false;
}

void AllLinksDialog::AddColumns()
{
    const HWND list = Item(IDC_LINKS_LIST);
    RECT client{};
    ::GetClientRect(list, &client);
    const int width = client.right - ::GetSystemMetricsForDpi(SM_CXVSCROLL, ::GetDpiForWindow(list));
    for (int column = 0; column < kColumnCount; ++column) {
        LVCOLUMNW item{};
        item.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        item.fmt = column == kSizeColumn ? LVCFMT_RIGHT : LVCFMT_LEFT;
        item.cx = width * kColumnPercent[static_cast<size_t>(column)] / 100;
        item.pszText = const_cast<wchar_t*>(kColumnTitles[static_cast<size_t>(column)]);  // not written to
        ListView_InsertColumn(list, column, &item);
    }
}

void AllLinksDialog::ShowRowTexts(int item, const LinkRow& row)
{
    const HWND list = Item(IDC_LINKS_LIST);
    const std::array<std::wstring, kColumnCount> texts = {
        base::ToUtf16(row.fileName), base::ToUtf16(LinkTypeText(row)), SizeText(row), base::ToUtf16(row.link.text),
        base::ToUtf16(row.link.url)};
    for (int column = 0; column < kColumnCount; ++column) {
        ListView_SetItemText(list, item, column, const_cast<wchar_t*>(texts[static_cast<size_t>(column)].c_str()));
    }
}

void AllLinksDialog::Refill()
{
    const HWND list = Item(IDC_LINKS_LIST);
    const std::vector<size_t> selectedBefore = SelectedRows();
    const std::set<size_t> keepSelected(selectedBefore.begin(), selectedBefore.end());

    shown_ = VisibleLinkRows(rows_, initial_.filter);
    if (sortColumn_ >= 0) {
        std::stable_sort(shown_.begin(), shown_.end(), [this](size_t a, size_t b) {
            return sortAscending_ ? RowLess(rows_[a], rows_[b], sortColumn_) : RowLess(rows_[b], rows_[a], sortColumn_);
        });
    }

    filling_ = true;
    ::SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    for (size_t position = 0; position < shown_.size(); ++position) {
        const size_t index = shown_[position];
        LVITEMW item{};
        item.mask = LVIF_PARAM | LVIF_TEXT;
        item.iItem = static_cast<int>(position);
        item.lParam = static_cast<LPARAM>(index);
        item.pszText = const_cast<wchar_t*>(L"");
        const int inserted = ListView_InsertItem(list, &item);
        ShowRowTexts(inserted, rows_[index]);
        ListView_SetCheckState(list, inserted, rows_[index].checked);
        if (keepSelected.contains(index)) {
            ListView_SetItemState(list, inserted, LVIS_SELECTED, LVIS_SELECTED);
        }
    }
    ::SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(list, nullptr, TRUE);
    filling_ = false;

    // The sort arrow on the sorted column.
    const HWND header = ListView_GetHeader(list);
    for (int column = 0; column < kColumnCount; ++column) {
        HDITEMW item{};
        item.mask = HDI_FORMAT;
        Header_GetItem(header, column, &item);
        item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (column == sortColumn_) {
            item.fmt |= sortAscending_ ? HDF_SORTUP : HDF_SORTDOWN;
        }
        Header_SetItem(header, column, &item);
    }
    UpdateCount();
}

void AllLinksDialog::UpdateCount()
{
    const size_t checked = static_cast<size_t>(
        std::count_if(shown_.begin(), shown_.end(), [this](size_t index) { return rows_[index].checked; }));
    const size_t pending = static_cast<size_t>(
        std::count_if(rows_.begin(), rows_.end(), [](const LinkRow& row) { return row.check == LinkCheck::Pending; }));
    std::wstring text = std::to_wstring(shown_.size()) + L" of " + std::to_wstring(rows_.size()) +
                        (rows_.size() == 1 ? L" link shown, " : L" links shown, ") + std::to_wstring(checked) +
                        L" checked.";
    if (pending > 0 && !checkingStopped_) {
        text += L"  Checking types and sizes: " + std::to_wstring(pending) + L" left.";
    }
    SetText(IDC_LINKS_COUNT, text);
    Enable(IDC_LINKS_EDIT, ListView_GetSelectedCount(Item(IDC_LINKS_LIST)) > 0);
}

void AllLinksDialog::UpdateSaveTo()
{
    Enable(IDC_LINKS_CATEGORY, Checked(IDC_LINKS_ONE_CATEGORY));
    const bool folder = Checked(IDC_LINKS_ONE_FOLDER);
    Enable(IDC_LINKS_FOLDER, folder);
    Enable(IDC_LINKS_BROWSE, folder);
}

std::vector<size_t> AllLinksDialog::SelectedRows() const
{
    const HWND list = Item(IDC_LINKS_LIST);
    std::vector<size_t> selected;
    for (int item = ListView_GetNextItem(list, -1, LVNI_SELECTED); item >= 0;
         item = ListView_GetNextItem(list, item, LVNI_SELECTED)) {
        if (static_cast<size_t>(item) < shown_.size()) {
            selected.push_back(shown_[static_cast<size_t>(item)]);
        }
    }
    return selected;
}

void AllLinksDialog::SetChecks(bool checked, bool selectedOnly)
{
    const HWND list = Item(IDC_LINKS_LIST);
    filling_ = true;
    for (size_t position = 0; position < shown_.size(); ++position) {
        const int item = static_cast<int>(position);
        if (selectedOnly && (ListView_GetItemState(list, item, LVIS_SELECTED) & LVIS_SELECTED) == 0) {
            continue;
        }
        rows_[shown_[position]].checked = checked;
        ListView_SetCheckState(list, item, checked);
    }
    filling_ = false;
    UpdateCount();
}

void AllLinksDialog::EditSelected()
{
    const std::vector<size_t> selected = SelectedRows();
    if (selected.empty()) {
        return;
    }
    LinkEditDialog::Values values;
    if (selected.size() == 1) {
        const LinkRow& row = rows_[selected.front()];
        values.name = base::ToUtf16(row.fileName);
        values.description = base::ToUtf16(row.description.value_or(initial_.useLinkText ? row.link.text : ""));
    }
    LinkEditDialog dialog(CurrentTheme(), selected.size(), std::move(values));
    const auto edited = dialog.Run(Handle());
    if (!edited) {
        return;
    }
    if (selected.size() == 1) {
        LinkRow& row = rows_[selected.front()];
        const std::string name = base::ToUtf8(edited->name);
        if (!name.empty() && name != row.fileName) {
            row.chosenName = core::SanitizeFileName(name);
            row.fileName = *row.chosenName;
        }
        row.description = base::ToUtf8(edited->description);
        row.checked = true;  // editing a link means wanting it
    } else {
        const std::string pattern = base::ToUtf8(edited->name);
        for (size_t i = 0; i < selected.size(); ++i) {
            LinkRow& row = rows_[selected[i]];
            if (auto name = NameFromPattern(pattern, i + 1, selected.size(), row.fileName)) {
                row.chosenName = *name;
                row.fileName = std::move(*name);
                row.checked = true;
            }
        }
    }
    Refill();
}

void AllLinksDialog::ShowListMenu(POINT screen)
{
    const HWND list = Item(IDC_LINKS_LIST);
    if (screen.x == -1 && screen.y == -1) {  // from the keyboard: at the focused row
        RECT rect{};
        const int focused = ListView_GetNextItem(list, -1, LVNI_FOCUSED);
        if (focused < 0 || !ListView_GetItemRect(list, focused, &rect, LVIR_LABEL)) {
            ::GetClientRect(list, &rect);
        }
        screen = POINT{rect.left, rect.bottom};
        ::ClientToScreen(list, &screen);
    }
    const bool any = ListView_GetSelectedCount(list) > 0;
    UniqueMenu menu(::CreatePopupMenu());
    const UINT flags = MF_STRING | (any ? 0U : static_cast<UINT>(MF_GRAYED));
    ::AppendMenuW(menu.get(), flags, ID_LINKS_CHECK_SELECTED, L"&Check selected links");
    ::AppendMenuW(menu.get(), flags, ID_LINKS_UNCHECK_SELECTED, L"&Uncheck selected links");
    ::AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu.get(), flags, ID_LINKS_EDIT, L"&Edit...");
    const auto command = static_cast<int>(
        ::TrackPopupMenuEx(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, Handle(), nullptr));
    switch (command) {
    case ID_LINKS_CHECK_SELECTED:
    case ID_LINKS_UNCHECK_SELECTED:
        SetChecks(command == ID_LINKS_CHECK_SELECTED, true);
        break;
    case ID_LINKS_EDIT:
        EditSelected();
        break;
    default:
        break;
    }
}

void AllLinksDialog::ApplyDetails(const std::vector<browser::LinkDetails>& items)
{
    if (Handle() == nullptr) {
        return;
    }
    bool changed = false;
    for (const browser::LinkDetails& details : items) {
        changed = ApplyLinkDetails(rows_, details) || changed;
    }
    if (!changed) {
        return;
    }
    // A link may have turned out to be a page, or a file: refill when the filter shows other rows.
    std::vector<size_t> visible = VisibleLinkRows(rows_, initial_.filter);
    std::vector<size_t> current = shown_;
    std::sort(visible.begin(), visible.end());
    std::sort(current.begin(), current.end());
    if (visible != current) {
        Refill();
        return;
    }
    for (size_t position = 0; position < shown_.size(); ++position) {
        ShowRowTexts(static_cast<int>(position), rows_[shown_[position]]);
    }
    UpdateCount();
}

void AllLinksDialog::StopChecking()
{
    checkingStopped_ = true;
    if (Handle() != nullptr) {
        UpdateCount();
    }
}

bool AllLinksDialog::Finish()
{
    Choice choice;
    for (const size_t index : shown_) {
        const LinkRow& row = rows_[index];
        if (!row.checked) {
            continue;
        }
        std::string description = row.description.value_or(initial_.useLinkText ? row.link.text : std::string());
        choice.picks.push_back(Pick{row.link.url, row.chosenName, std::move(description)});
    }
    if (choice.picks.empty()) {
        Warn(L"Check the links you want to download.");
        return false;
    }
    if (Checked(IDC_LINKS_ONE_CATEGORY)) {
        const HWND combo = Item(IDC_LINKS_CATEGORY);
        const int index = ComboBox_GetCurSel(combo);
        if (index < 0) {
            Warn(L"Choose a category.");
            return false;
        }
        choice.saveTo = SaveTo::OneCategory;
        choice.category = model::CategoryId{static_cast<std::int64_t>(ComboBox_GetItemData(combo, index))};
    } else if (Checked(IDC_LINKS_ONE_FOLDER)) {
        const std::wstring folder = Text(IDC_LINKS_FOLDER);
        if (folder.empty() || !std::filesystem::path(folder).is_absolute()) {
            Warn(L"Enter the full path of the folder to save the files to.");
            ::SetFocus(Item(IDC_LINKS_FOLDER));
            return false;
        }
        choice.saveTo = SaveTo::OneFolder;
        choice.folder = base::ToUtf8(folder);
    }
    result_ = std::move(choice);
    return true;
}

bool AllLinksDialog::OnCommand(int id, int code, HWND control)
{
    switch (id) {
    case IDC_LINKS_BY_CATEGORY:
    case IDC_LINKS_ONE_CATEGORY:
    case IDC_LINKS_ONE_FOLDER:
        UpdateSaveTo();
        return true;
    case IDC_LINKS_BROWSE:
        if (auto folder = PickFolder(Handle(), Text(IDC_LINKS_FOLDER))) {
            SetText(IDC_LINKS_FOLDER, *folder);
        }
        return true;
    case IDC_LINKS_CHECK_ALL:
    case IDC_LINKS_UNCHECK_ALL:
        SetChecks(id == IDC_LINKS_CHECK_ALL, false);
        return true;
    case IDC_LINKS_EDIT:
        EditSelected();
        return true;
    case IDC_LINKS_USE_TEXT:
        initial_.useLinkText = Checked(IDC_LINKS_USE_TEXT);
        return true;
    case IDC_LINKS_HIDE_HTML:
    case IDC_LINKS_HIDE_IMAGES:
    case IDC_LINKS_HIDE_REPEATED:
        initial_.filter.hideHtml = Checked(IDC_LINKS_HIDE_HTML);
        initial_.filter.hideImages = Checked(IDC_LINKS_HIDE_IMAGES);
        initial_.filter.hideRepeated = Checked(IDC_LINKS_HIDE_REPEATED);
        Refill();
        return true;
    case IDOK:
        if (Finish()) {
            Close(IDOK);
        }
        return true;
    default:
        return Dialog::OnCommand(id, code, control);
    }
}

bool AllLinksDialog::OnNotify(const NMHDR& header, LRESULT& result)
{
    if (header.idFrom != IDC_LINKS_LIST) {
        return false;
    }
    switch (header.code) {
    case LVN_ITEMCHANGED: {
        const auto& change = reinterpret_cast<const NMLISTVIEW&>(header);
        if (filling_ || change.iItem < 0 || (change.uChanged & LVIF_STATE) == 0) {
            break;
        }
        if (((change.uNewState ^ change.uOldState) & LVIS_STATEIMAGEMASK) != 0 &&
            static_cast<size_t>(change.iItem) < shown_.size()) {
            rows_[shown_[static_cast<size_t>(change.iItem)]].checked =
                ((change.uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;  // image 2 is the check mark
        }
        UpdateCount();
        break;
    }
    case LVN_COLUMNCLICK: {
        const int column = reinterpret_cast<const NMLISTVIEW&>(header).iSubItem;
        sortAscending_ = column == sortColumn_ ? !sortAscending_ : true;
        sortColumn_ = column;
        Refill();
        break;
    }
    case NM_DBLCLK:
        EditSelected();
        break;
    default:
        return false;
    }
    result = 0;
    return true;
}

bool AllLinksDialog::OnMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
{
    if (message != WM_CONTEXTMENU || reinterpret_cast<HWND>(wParam) != Item(IDC_LINKS_LIST)) {
        return false;
    }
    ShowListMenu(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
    result = 0;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Edit links

LinkEditDialog::LinkEditDialog(const Theme& theme, size_t count, Values values)
    : Dialog(IDD_LINK_EDIT, theme), count_(count), values_(std::move(values))
{
}

std::optional<LinkEditDialog::Values> LinkEditDialog::Run(HWND owner)
{
    RunModal(owner);
    return result_;
}

bool LinkEditDialog::OnInit()
{
    SetText(IDC_LINK_EDIT_NAME, values_.name);
    SetText(IDC_LINK_EDIT_DESC, values_.description);
    if (count_ > 1) {
        ::SetWindowTextW(Handle(), (L"Rename " + std::to_wstring(count_) + L" links").c_str());
        SetText(IDC_LINK_EDIT_NAME_LABEL, L"&File name pattern:");
        SetText(IDC_LINK_EDIT_HINT, L"* becomes a number in list order: photo* names them photo1, photo2 and so on, "
                                    L"each keeping its own extension unless the pattern gives one.");
        Show(IDC_LINK_EDIT_DESC_LABEL, false);
        Show(IDC_LINK_EDIT_DESC, false);
    } else {
        SetText(IDC_LINK_EDIT_HINT, L"Leave the name as it is to use the one the server sends.");
    }
    ::SetFocus(Item(IDC_LINK_EDIT_NAME));
    ::SendMessageW(Item(IDC_LINK_EDIT_NAME), EM_SETSEL, 0, -1);
    return false;
}

bool LinkEditDialog::OnCommand(int id, int code, HWND control)
{
    if (id != IDOK) {
        return Dialog::OnCommand(id, code, control);
    }
    Values values{Text(IDC_LINK_EDIT_NAME), Text(IDC_LINK_EDIT_DESC)};
    if (count_ > 1 && !NameFromPattern(base::ToUtf8(values.name), 1, count_, "")) {
        Warn(L"The pattern needs exactly one *, which becomes the number.");
        return true;
    }
    result_ = std::move(values);
    Close(IDOK);
    return true;
}

}  // namespace ldm::ui
