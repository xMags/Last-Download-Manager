#include "ui/dialogs/GrabberWindow.h"

#include "base/Utf.h"
#include "core/Url.h"
#include "resource.h"
#include "ui/Format.h"
#include "ui/dialogs/MessageDialog.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <unordered_set>

namespace ldm::ui {

namespace {

enum Column {
    kName,
    kSize,
    kType,
    kStatus,
    kAddress,
    kLinkText,
    kColumnCount,
};

struct ColumnInfo {
    const wchar_t* title;
    int width;
};

constexpr std::array<ColumnInfo, kColumnCount> kColumns{{
    {L"Name", 200},
    {L"Size", 75},
    {L"Type", 50},
    {L"Status", 110},
    {L"Address", 260},
    {L"Link text", 160},
}};

constexpr int kStateUnchecked = 1;
constexpr int kStateChecked = 2;

int Scale(HWND window, int value96)
{
    return ::MulDiv(value96, static_cast<int>(::GetDpiForWindow(window)), 96);
}

}  // namespace

GrabberWindow::GrabberWindow(const Theme& theme, model::GrabberProjectId project, Callbacks callbacks)
    : Dialog(IDD_GRABBER, theme), project_(project), callbacks_(std::move(callbacks))
{
}

void GrabberWindow::Open(HWND owner, std::shared_ptr<const grabber::GrabberSnapshot> snapshot)
{
    snapshot_ = std::move(snapshot);
    // Owned by nothing, so it has its own taskbar button and stays up when the main window hides.
    (void)owner;
    CreateModeless(nullptr);
    if (Handle() == nullptr) {
        return;
    }
    ::ShowWindow(Handle(), SW_SHOW);
    Post(core::GrabberOpenCommand{project_, true});
}

void GrabberWindow::Activate()
{
    if (Handle() == nullptr) {
        return;
    }
    if (::IsIconic(Handle())) {
        ::ShowWindow(Handle(), SW_RESTORE);
    }
    ::SetForegroundWindow(Handle());
}

const grabber::GrabberProjectView* GrabberWindow::View() const
{
    if (!snapshot_) {
        return nullptr;
    }
    for (const grabber::GrabberProjectView& view : snapshot_->projects) {
        if (view.project.id == project_) {
            return &view;
        }
    }
    return nullptr;
}

bool GrabberWindow::OnInit()
{
    ::SendMessageW(Handle(), WM_SETICON, ICON_BIG,
                   reinterpret_cast<LPARAM>(::LoadIconW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_LDM))));
    ::SendMessageW(Handle(), WM_SETICON, ICON_SMALL,
                   reinterpret_cast<LPARAM>(::LoadIconW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_LDM))));
    const HWND list = Item(IDC_GG_LIST);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES);
    for (int i = 0; i < kColumnCount; ++i) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<wchar_t*>(kColumns[static_cast<std::size_t>(i)].title);  // copied by the list view
        column.cx = Scale(Handle(), kColumns[static_cast<std::size_t>(i)].width);
        if (i == kSize) {
            column.mask |= LVCF_FMT;
            column.fmt = LVCFMT_RIGHT;
        }
        ListView_InsertColumn(list, i, &column);
    }
    const HWND tree = Item(IDC_GG_TREE);
    if (CurrentTheme().IsDark()) {
        ListView_SetBkColor(list, CurrentTheme().Colors().window);
        ListView_SetTextBkColor(list, CurrentTheme().Colors().window);
        ListView_SetTextColor(list, CurrentTheme().Colors().text);
        TreeView_SetBkColor(tree, CurrentTheme().Colors().window);
        TreeView_SetTextColor(tree, CurrentTheme().Colors().text);
    }
    // The buttons keep their place; everything below them follows the window's size.
    ::GetWindowRect(Item(IDC_GG_EXPLORE), &buttonsBottom_);
    ::MapWindowPoints(nullptr, Handle(), reinterpret_cast<POINT*>(&buttonsBottom_), 2);

    RECT work{};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int width = std::min(Scale(Handle(), 1100), static_cast<int>(work.right - work.left));
    const int height = std::min(Scale(Handle(), 640), static_cast<int>(work.bottom - work.top));
    ::SetWindowPos(Handle(), nullptr, work.left + (work.right - work.left - width) / 2,
                   work.top + (work.bottom - work.top - height) / 2, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    RefreshTree();
    RefreshRows();
    RefreshButtons();
    RefreshStatus();
    Layout();
    return true;
}

void GrabberWindow::Layout()
{
    RECT client{};
    ::GetClientRect(Handle(), &client);
    const int margin = Scale(Handle(), 10);
    const int gap = Scale(Handle(), 6);
    const int statusHeight = Scale(Handle(), 18);
    const int top = buttonsBottom_.bottom + gap;
    const int bottom = client.bottom - margin - statusHeight;
    const int treeWidth = std::clamp(static_cast<int>(client.right / 5), Scale(Handle(), 150), Scale(Handle(), 260));
    const int height = std::max(0, bottom - top);
    ::MoveWindow(Item(IDC_GG_TREE), margin, top, treeWidth, height, TRUE);
    ::MoveWindow(Item(IDC_GG_LIST), margin + treeWidth + gap, top,
                 std::max(0, static_cast<int>(client.right) - margin * 2 - treeWidth - gap), height, TRUE);
    ::MoveWindow(Item(IDC_GG_STATUS), margin, bottom + Scale(Handle(), 4), std::max(0, static_cast<int>(client.right) - margin * 2),
                 statusHeight - Scale(Handle(), 4), TRUE);
}

void GrabberWindow::Update(std::shared_ptr<const grabber::GrabberSnapshot> snapshot)
{
    snapshot_ = std::move(snapshot);
    if (Handle() == nullptr) {
        return;
    }
    const grabber::GrabberProjectView* view = View();
    if (view == nullptr) {
        // A new project shows in a later snapshot; one that was shown and is gone was deleted.
        if (seen_) {
            ::DestroyWindow(Handle());
        }
        return;
    }
    seen_ = true;
    const std::wstring title = L"Site Grabber - " + base::ToUtf16(view->project.name);
    wchar_t current[512] = {};
    ::GetWindowTextW(Handle(), current, 512);
    if (title != current) {
        ::SetWindowTextW(Handle(), title.c_str());
    }
    RefreshTree();
    RefreshRows();
    RefreshButtons();
    RefreshStatus();
}

void GrabberWindow::UpdateDownloads(const core::Snapshot& snapshot)
{
    const grabber::GrabberProjectView* view = View();
    if (Handle() == nullptr || view == nullptr || !view->downloading) {
        if (!downloads_.empty()) {
            downloads_.clear();
        }
        return;
    }
    downloads_.clear();
    for (const auto& row : snapshot.downloads) {
        if (row->download.projectId == project_) {
            downloads_.emplace(row->download.id.value, row);
        }
    }
    ::InvalidateRect(Item(IDC_GG_LIST), nullptr, FALSE);
}

void GrabberWindow::RefreshTree()
{
    const grabber::GrabberProjectView* view = View();
    const std::vector<std::wstring> types = view ? GrabberTypes(view->items) : std::vector<std::wstring>{};
    const HWND tree = Item(IDC_GG_TREE);
    if (!treeNodes_.empty() && types == typeNodes_) {
        return;
    }
    refreshingTree_ = true;
    typeNodes_ = types;
    treeNodes_.clear();
    TreeView_DeleteAllItems(tree);
    HTREEITEM select = nullptr;
    const auto add = [&](HTREEITEM parent, const std::wstring& text, std::optional<GrabberNode> node) {
        TVINSERTSTRUCTW insert{};
        insert.hParent = parent;
        insert.hInsertAfter = TVI_LAST;
        insert.item.mask = TVIF_TEXT | TVIF_PARAM;
        insert.item.pszText = const_cast<wchar_t*>(text.c_str());  // copied by the tree
        insert.item.lParam = node ? static_cast<LPARAM>(treeNodes_.size()) : -1;
        if (node) {
            treeNodes_.push_back(*node);
        }
        const HTREEITEM item = TreeView_InsertItem(tree, &insert);
        if (node && *node == node_) {
            select = item;
        }
        return item;
    };
    add(TVI_ROOT, L"All files", GrabberNode{GrabberNode::Kind::AllFiles, {}});
    add(TVI_ROOT, L"Checked", GrabberNode{GrabberNode::Kind::Checked, {}});
    add(TVI_ROOT, L"Downloaded", GrabberNode{GrabberNode::Kind::Downloaded, {}});
    add(TVI_ROOT, L"Failed", GrabberNode{GrabberNode::Kind::Failed, {}});
    const HTREEITEM byType = add(TVI_ROOT, L"By type", std::nullopt);
    for (const std::wstring& type : types) {
        add(byType, type.empty() ? L"(no extension)" : type, GrabberNode{GrabberNode::Kind::Type, type});
    }
    add(TVI_ROOT, L"Other sites", GrabberNode{GrabberNode::Kind::OtherSites, {}});
    add(TVI_ROOT, L"Left out by the filters", GrabberNode{GrabberNode::Kind::LeftOut, {}});
    add(TVI_ROOT, L"Pages", GrabberNode{GrabberNode::Kind::Pages, {}});
    TreeView_Expand(tree, byType, TVE_EXPAND);
    if (select == nullptr) {
        node_ = GrabberNode{};
        select = TreeView_GetRoot(tree);
    }
    TreeView_SelectItem(tree, select);
    refreshingTree_ = false;
}

void GrabberWindow::RefreshRows()
{
    const HWND list = Item(IDC_GG_LIST);
    // The list view keeps its selection by row, and rows move with every snapshot and node change,
    // so the selection follows the items: actions never land on the files that moved into its rows.
    std::unordered_set<std::int64_t> selected;
    for (int row = ListView_GetNextItem(list, -1, LVNI_SELECTED); row >= 0; row = ListView_GetNextItem(list, row, LVNI_SELECTED)) {
        if (static_cast<std::size_t>(row) < rowIds_.size()) {
            selected.insert(rowIds_[static_cast<std::size_t>(row)]);
        }
    }
    const int focusedRow = ListView_GetNextItem(list, -1, LVNI_FOCUSED);
    const std::optional<std::int64_t> focused =
        focusedRow >= 0 && static_cast<std::size_t>(focusedRow) < rowIds_.size()
            ? std::optional(rowIds_[static_cast<std::size_t>(focusedRow)])
            : std::nullopt;

    const grabber::GrabberProjectView* view = View();
    rows_.clear();
    rowIds_.clear();
    if (view != nullptr) {
        const auto start = core::ParseUrl(view->project.definition.startPage);
        const std::string host = start ? start->host : std::string();
        for (std::size_t i = 0; i < view->items.size(); ++i) {
            if (InGrabberNode(*view->items[i], node_, host)) {
                rows_.push_back(i);
                rowIds_.push_back(view->items[i]->id);
            }
        }
    }
    ListView_SetItemCountEx(list, static_cast<int>(rows_.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (!selected.empty() || focused) {
        for (std::size_t row = 0; row < rowIds_.size(); ++row) {
            UINT state = selected.contains(rowIds_[row]) ? LVIS_SELECTED : 0;
            if (focused && *focused == rowIds_[row]) {
                state |= LVIS_FOCUSED;
            }
            if (state != 0) {
                ListView_SetItemState(list, static_cast<int>(row), state, state);
            }
        }
    }
    ::InvalidateRect(list, nullptr, FALSE);
}

void GrabberWindow::RefreshButtons()
{
    const grabber::GrabberProjectView* view = View();
    const bool usable = view != nullptr && !view->damaged;
    const bool busy = usable && (view->exploring || view->downloading);
    Enable(IDC_GG_EXPLORE, usable && !view->exploring);
    Enable(IDC_GG_UPDATE, usable && !view->exploring);
    Enable(IDC_GG_STOP, busy);
    Enable(IDC_GG_DOWNLOAD, usable && !view->downloading && view->stats.filesChecked > 0);
    Enable(IDC_GG_STOP_DOWNLOADS, usable && view->downloading);
    Enable(IDC_GG_ADD_MAIN, usable && view->stats.filesChecked > 0);
    Enable(IDC_GG_EDIT, usable && !busy);
    Enable(IDC_GG_TEMPLATE, usable);
}

void GrabberWindow::RefreshStatus()
{
    const grabber::GrabberProjectView* view = View();
    const std::wstring text = view ? GrabberStatusText(*view) : std::wstring();
    if (Text(IDC_GG_STATUS) != text) {
        SetText(IDC_GG_STATUS, text);
    }
}

const model::GrabberItem* GrabberWindow::RowItem(int row) const
{
    const grabber::GrabberProjectView* view = View();
    if (view == nullptr || row < 0 || static_cast<std::size_t>(row) >= rows_.size() ||
        rows_[static_cast<std::size_t>(row)] >= view->items.size()) {
        return nullptr;
    }
    return view->items[rows_[static_cast<std::size_t>(row)]].get();
}

std::vector<int> GrabberWindow::SelectedRows() const
{
    std::vector<int> rows;
    const HWND list = Item(IDC_GG_LIST);
    for (int row = ListView_GetNextItem(list, -1, LVNI_SELECTED); row >= 0; row = ListView_GetNextItem(list, row, LVNI_SELECTED)) {
        rows.push_back(row);
    }
    return rows;
}

std::wstring GrabberWindow::Cell(const model::GrabberItem& item, int column) const
{
    switch (column) {
    case kName:
        return GrabberItemName(item);
    case kSize:
        return item.size ? FormatSize(*item.size) : std::wstring();
    case kType:
        return GrabberItemType(item);
    case kStatus: {
        const core::DownloadView* download = nullptr;
        if (item.downloadId) {
            if (const auto found = downloads_.find(item.downloadId->value); found != downloads_.end()) {
                download = found->second.get();
            }
        }
        return GrabberItemStatus(item, download);
    }
    case kAddress:
        return base::ToUtf16(item.url);
    case kLinkText:
        return base::ToUtf16(item.linkText);
    default:
        return std::wstring();
    }
}

void GrabberWindow::Post(core::GrabberCommand command)
{
    if (callbacks_.post) {
        callbacks_.post(std::move(command));
    }
}

void GrabberWindow::CheckRows(const std::vector<int>& rows, bool checked)
{
    core::GrabberCheckCommand command;
    command.id = project_;
    command.checked = checked;
    for (const int row : rows) {
        if (const model::GrabberItem* item = RowItem(row); item != nullptr && item->kind == model::GrabberItemKind::File) {
            command.items.push_back(item->id);
        }
    }
    if (!command.items.empty()) {
        Post(std::move(command));
    }
}

void GrabberWindow::ShowStatistics()
{
    if (const grabber::GrabberProjectView* view = View()) {
        (void)ShowMessage(Handle(), CurrentTheme(), MessageRequest{GrabberStatisticsText(*view), MessageKind::Information});
    }
}

void GrabberWindow::ShowListMenu(POINT screen)
{
    // What the menu acts on is copied now: the menu runs its own message loop, during which newer
    // snapshots replace the items and the rows.
    std::vector<std::int64_t> items;
    for (const int row : SelectedRows()) {
        if (const model::GrabberItem* item = RowItem(row)) {
            items.push_back(item->id);
        }
    }
    const model::GrabberItem* single = items.size() == 1 ? RowItem(SelectedRows().front()) : nullptr;
    const bool done = single != nullptr && single->state == model::GrabberItemState::Done && !single->localPath.empty();
    const std::filesystem::path localPath = done ? std::filesystem::path(base::ToUtf16(single->localPath)) : std::filesystem::path();
    const bool any = !items.empty();
    HMENU menu = ::CreatePopupMenu();
    const auto add = [menu](UINT id, const wchar_t* text, bool enabled) {
        ::AppendMenuW(menu, MF_STRING | (enabled ? 0 : MF_GRAYED), id, text);
    };
    add(ID_GRABBER_CHECK, L"&Check", any);
    add(ID_GRABBER_UNCHECK, L"&Uncheck", any);
    add(ID_GRABBER_CHECK_ALL, L"Check &all", true);
    add(ID_GRABBER_UNCHECK_ALL, L"U&ncheck all", true);
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(ID_GRABBER_OPEN, L"&Open", done);
    add(ID_GRABBER_OPEN_FOLDER, L"Open &folder", done);
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(ID_GRABBER_ADD_MAIN, L"Add to the &main list", any);
    if (screen.x == -1 && screen.y == -1) {
        RECT rect{};
        ::GetWindowRect(Item(IDC_GG_LIST), &rect);
        screen = POINT{rect.left + 10, rect.top + 10};
    }
    const int command = static_cast<int>(::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, Handle(), nullptr));
    ::DestroyMenu(menu);
    switch (command) {
    case ID_GRABBER_CHECK:
    case ID_GRABBER_UNCHECK:
        if (any) {
            Post(core::GrabberCheckCommand{project_, items, false, command == ID_GRABBER_CHECK});
        }
        break;
    case ID_GRABBER_CHECK_ALL:
    case ID_GRABBER_UNCHECK_ALL: {
        core::GrabberCheckCommand all;
        all.id = project_;
        all.all = true;
        all.checked = command == ID_GRABBER_CHECK_ALL;
        Post(std::move(all));
        break;
    }
    case ID_GRABBER_OPEN:
        if (done && callbacks_.openFile) {
            callbacks_.openFile(localPath);
        }
        break;
    case ID_GRABBER_OPEN_FOLDER:
        if (done && callbacks_.showInFolder) {
            callbacks_.showInFolder(localPath);
        }
        break;
    case ID_GRABBER_ADD_MAIN:
        if (any) {
            Post(core::GrabberAddToMainListCommand{project_, items});
        }
        break;
    default:
        break;
    }
}

bool GrabberWindow::OnCommand(int id, int code, HWND control)
{
    switch (id) {
    case IDC_GG_EXPLORE:
    case IDC_GG_UPDATE:
        if (callbacks_.explore) {
            callbacks_.explore(project_, id == IDC_GG_UPDATE);
        }
        return true;
    case IDC_GG_STOP:
        Post(core::GrabberStopCommand{project_});
        return true;
    case IDC_GG_DOWNLOAD:
        Post(core::GrabberDownloadCommand{project_});
        return true;
    case IDC_GG_STOP_DOWNLOADS:
        Post(core::GrabberStopDownloadsCommand{project_});
        return true;
    case IDC_GG_ADD_MAIN:
        Post(core::GrabberAddToMainListCommand{project_, {}});
        return true;
    case IDC_GG_EDIT:
        if (callbacks_.edit) {
            callbacks_.edit(project_);
        }
        return true;
    case IDC_GG_TEMPLATE:
        if (callbacks_.saveAsTemplate) {
            callbacks_.saveAsTemplate(project_);
        }
        return true;
    case IDC_GG_STATS:
        ShowStatistics();
        return true;
    case IDOK:
        return true;  // Enter in the list does not close the window
    default:
        return Dialog::OnCommand(id, code, control);
    }
}

bool GrabberWindow::OnNotify(const NMHDR& header, LRESULT& result)
{
    if (header.idFrom == IDC_GG_TREE && header.code == TVN_SELCHANGEDW) {
        const auto& changed = reinterpret_cast<const NMTREEVIEWW&>(header);
        if (!refreshingTree_ && changed.itemNew.lParam >= 0 &&
            static_cast<std::size_t>(changed.itemNew.lParam) < treeNodes_.size()) {
            node_ = treeNodes_[static_cast<std::size_t>(changed.itemNew.lParam)];
            RefreshRows();
        }
        result = 0;
        return true;
    }
    if (header.idFrom != IDC_GG_LIST) {
        return false;
    }
    switch (header.code) {
    case LVN_GETDISPINFOW: {
        auto& info = const_cast<NMLVDISPINFOW&>(reinterpret_cast<const NMLVDISPINFOW&>(header));
        const model::GrabberItem* item = RowItem(info.item.iItem);
        if (item == nullptr) {
            result = 0;
            return true;
        }
        if ((info.item.mask & LVIF_TEXT) != 0) {
            cellText_ = Cell(*item, info.item.iSubItem);
            info.item.pszText = const_cast<wchar_t*>(cellText_.c_str());  // read before the next notification
        }
        if ((info.item.mask & LVIF_STATE) != 0) {
            // Pages have no check box.
            const int image = item->kind == model::GrabberItemKind::Page ? 0 : item->checked ? kStateChecked : kStateUnchecked;
            info.item.state = INDEXTOSTATEIMAGEMASK(image);
            info.item.stateMask = LVIS_STATEIMAGEMASK;
        }
        result = 0;
        return true;
    }
    case NM_CLICK: {
        const auto& click = reinterpret_cast<const NMITEMACTIVATE&>(header);
        LVHITTESTINFO hit{};
        hit.pt = click.ptAction;
        if (ListView_SubItemHitTest(Item(IDC_GG_LIST), &hit) >= 0 && (hit.flags & LVHT_ONITEMSTATEICON) != 0) {
            if (const model::GrabberItem* item = RowItem(hit.iItem)) {
                CheckRows({hit.iItem}, !item->checked);
            }
        }
        result = 0;
        return true;
    }
    case LVN_KEYDOWN: {
        const auto& key = reinterpret_cast<const NMLVKEYDOWN&>(header);
        if (key.wVKey == VK_SPACE) {
            const std::vector<int> rows = SelectedRows();
            if (const model::GrabberItem* first = rows.empty() ? nullptr : RowItem(rows.front())) {
                CheckRows(rows, !first->checked);
            }
        } else if (key.wVKey == 'A' && (::GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            ListView_SetItemState(Item(IDC_GG_LIST), -1, LVIS_SELECTED, LVIS_SELECTED);
        }
        result = 0;
        return true;
    }
    case NM_DBLCLK: {
        const std::vector<int> rows = SelectedRows();
        const model::GrabberItem* item = rows.size() == 1 ? RowItem(rows.front()) : nullptr;
        if (item != nullptr && item->state == model::GrabberItemState::Done && !item->localPath.empty() && callbacks_.openFile) {
            callbacks_.openFile(std::filesystem::path(base::ToUtf16(item->localPath)));
        }
        result = 0;
        return true;
    }
    default:
        return false;
    }
}

bool GrabberWindow::OnMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
{
    switch (message) {
    case WM_SIZE:
        Layout();
        result = 0;
        return true;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize = POINT{Scale(Handle(), 700), Scale(Handle(), 360)};
        result = 0;
        return true;
    }
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wParam) == Item(IDC_GG_LIST)) {
            ShowListMenu(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            result = 0;
            return true;
        }
        return false;
    default:
        return false;
    }
}

void GrabberWindow::OnDestroyed()
{
    // The explorer stops publishing the items and adds the checked files to the main list when
    // the project asks for it.
    Post(core::GrabberOpenCommand{project_, false});
    if (callbacks_.closed) {
        callbacks_.closed(project_);
    }
}

}  // namespace ldm::ui
