#include "ui/DownloadListView.h"

#include "base/Utf.h"
#include "core/FileNames.h"
#include "ui/Dpi.h"
#include "ui/Format.h"
#include "ui/GlyphIcons.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <set>

namespace ldm::ui {

namespace {

constexpr int kQueueMark96 = 12;
constexpr UINT_PTR kListSubclassId = 1;

}  // namespace

void DownloadListView::Create(HWND parent, Callbacks callbacks)
{
    callbacks_ = std::move(callbacks);
    list_ = ::CreateWindowExW(0, WC_LISTVIEWW, nullptr,
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS |
                                  LVS_SHAREIMAGELISTS,
                              0, 0, 0, 0, parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP |
                                                 LVS_EX_LABELTIP | LVS_EX_SUBITEMIMAGES);
    // The shell's own small icons, so every file type looks as it does in Explorer.
    SHFILEINFOW info{};
    const auto systemImages = reinterpret_cast<HIMAGELIST>(::SHGetFileInfoW(  // documented to return the list
        L".txt", FILE_ATTRIBUTE_NORMAL, &info, sizeof(info), SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON));
    ListView_SetImageList(list_, systemImages, LVSIL_SMALL);
    columns_ = DefaultColumns();
    ApplyColumns();

    queueTip_ = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                  CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, list_, nullptr,
                                  ::GetModuleHandleW(nullptr), nullptr);
    TOOLINFOW tool{};
    tool.cbSize = sizeof(tool);
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = list_;
    tool.uId = reinterpret_cast<UINT_PTR>(list_);  // the documented form for TTF_IDISHWND
    tool.lpszText = LPSTR_TEXTCALLBACKW;
    ::SendMessageW(queueTip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    ::SetWindowSubclass(list_, &DownloadListView::ListSubclass, kListSubclassId, reinterpret_cast<DWORD_PTR>(this));
}

std::wstring DownloadListView::QueueTipAt(POINT client) const
{
    LVHITTESTINFO hit{};
    hit.pt = client;
    if (!snapshot_ || ListView_SubItemHitTest(list_, &hit) < 0 || hit.iItem < 0 ||
        static_cast<size_t>(hit.iItem) >= index_.size() || hit.iSubItem < 0 ||
        static_cast<size_t>(hit.iSubItem) >= shown_.size() || shown_[static_cast<size_t>(hit.iSubItem)] != Column::Queue) {
        return {};
    }
    const model::Queue* queue = QueueOf(*snapshot_->downloads[index_[static_cast<size_t>(hit.iItem)]]);
    return queue != nullptr ? L"Queue: " + base::ToUtf16(queue->name) : std::wstring();
}

LRESULT CALLBACK DownloadListView::ListSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                               DWORD_PTR data)
{
    auto* self = reinterpret_cast<DownloadListView*>(data);  // set with the subclass, alive as long as the list
    switch (message) {
    case WM_MOUSEMOVE: {
        std::wstring text = self->QueueTipAt(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        if (text != self->queueTipText_) {
            self->queueTipText_ = std::move(text);
            ::SendMessageW(self->queueTip_, TTM_POP, 0, 0);  // shows again, with the new text, when the mouse rests
        }
        break;
    }
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (header->hwndFrom == self->queueTip_ && header->code == TTN_GETDISPINFOW) {
            auto* info = reinterpret_cast<NMTTDISPINFOW*>(lParam);
            info->lpszText = const_cast<wchar_t*>(self->queueTipText_.c_str());  // empty: no tip
            return 0;
        }
        break;
    }
    case WM_NCDESTROY:
        ::RemoveWindowSubclass(window, &DownloadListView::ListSubclass, id);
        break;
    default:
        break;
    }
    return ::DefSubclassProc(window, message, wParam, lParam);
}

void DownloadListView::SetColumns(const std::vector<ColumnState>& columns)
{
    columns_ = columns;
    ApplyColumns();
}

void DownloadListView::ApplyColumns()
{
    while (ListView_DeleteColumn(list_, 0)) {
    }
    shown_.clear();
    chosenWidths96_.clear();
    const UINT dpi = DpiOf(list_);
    for (const ColumnState& column : columns_) {
        if (!column.visible) {
            continue;
        }
        chosenWidths96_[column.id] = column.width;
        std::wstring title(ColumnTitle(column.id));
        LVCOLUMNW data{};
        data.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        data.fmt = LVCFMT_LEFT;  // every column, sizes and rates included, aligns to the left
        data.cx = Scale(column.width, dpi);
        data.pszText = title.data();
        ListView_InsertColumn(list_, static_cast<int>(shown_.size()), &data);
        shown_.push_back(column.id);
    }
    UpdateSortArrow();
    FitLastColumn();
}

void DownloadListView::FitLastColumn()
{
    const int count = static_cast<int>(shown_.size());
    if (list_ == nullptr || count == 0 || fitting_) {
        return;
    }
    std::vector<int> order(static_cast<size_t>(count));
    ListView_GetColumnOrderArray(list_, count, order.data());
    const UINT dpi = DpiOf(list_);
    int others = 0;
    for (int i = 0; i + 1 < count; ++i) {
        others += ListView_GetColumnWidth(list_, order[static_cast<size_t>(i)]);
    }
    const int last = order.back();
    const auto chosen = chosenWidths96_.find(shown_[static_cast<size_t>(last)]);
    const int chosenWidth = Scale(chosen != chosenWidths96_.end() ? chosen->second : 100, dpi);
    RECT client{};
    ::GetClientRect(list_, &client);  // excludes a visible vertical scroll bar
    const int width = std::max(chosenWidth, static_cast<int>(client.right) - others);
    if (width != ListView_GetColumnWidth(list_, last)) {
        fitting_ = true;
        ListView_SetColumnWidth(list_, last, width);
        fitting_ = false;
    }
}

std::vector<ColumnState> DownloadListView::Columns() const
{
    const int count = static_cast<int>(shown_.size());
    std::vector<int> order(static_cast<size_t>(count));
    ListView_GetColumnOrderArray(list_, count, order.data());
    const UINT dpi = DpiOf(list_);
    std::vector<ColumnState> result;
    for (const int position : order) {
        const Column id = shown_[static_cast<size_t>(position)];
        const auto chosen = chosenWidths96_.find(id);
        const int width = chosen != chosenWidths96_.end() ? chosen->second : Unscale(ListView_GetColumnWidth(list_, position), dpi);
        result.push_back(ColumnState{id, width, true});
    }
    for (const ColumnState& column : columns_) {
        if (!column.visible) {
            result.push_back(column);
        }
    }
    return result;
}

void DownloadListView::UpdateSortArrow()
{
    const HWND header = ListView_GetHeader(list_);
    for (size_t i = 0; i < shown_.size(); ++i) {
        HDITEMW item{};
        item.mask = HDI_FORMAT;
        Header_GetItem(header, static_cast<int>(i), &item);
        item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (SortKeyForColumn(shown_[i]) == sort_.key && sort_.key != SortKey::OrderOfAddition) {
            item.fmt |= sort_.descending ? HDF_SORTDOWN : HDF_SORTUP;
        }
        Header_SetItem(header, static_cast<int>(i), &item);
    }
}

void DownloadListView::SetSnapshot(std::shared_ptr<const core::Snapshot> snapshot)
{
    const std::vector<model::DownloadId> selected = SelectedIds();
    std::optional<model::DownloadId> focused;
    if (const auto position = FocusedPosition(); position && snapshot_) {
        focused = snapshot_->downloads[index_[*position]]->download.id;
    }
    snapshot_ = std::move(snapshot);
    Reindex();

    // Put the selection back on the same downloads, wherever they moved.
    updating_ = true;
    const std::set<model::DownloadId> wanted(selected.begin(), selected.end());
    ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    for (size_t row = 0; row < index_.size(); ++row) {
        const model::DownloadId id = snapshot_->downloads[index_[row]]->download.id;
        UINT state = wanted.contains(id) ? LVIS_SELECTED : 0;
        if (focused && id == *focused) {
            state |= LVIS_FOCUSED;
        }
        if (state != 0) {
            ListView_SetItemState(list_, static_cast<int>(row), state, LVIS_SELECTED | LVIS_FOCUSED);
        }
    }
    updating_ = false;
    if (callbacks_.selectionChanged) {
        callbacks_.selectionChanged();  // status changes alter what the toolbar allows
    }
}

void DownloadListView::Reindex()
{
    if (!snapshot_) {
        return;
    }
    index_ = BuildIndex(*snapshot_, filter_, sort_);
    ListView_SetItemCountEx(list_, static_cast<int>(index_.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    ::InvalidateRect(list_, nullptr, FALSE);
    FitLastColumn();  // the vertical scroll bar may have come or gone
}

void DownloadListView::SetFilter(const ListFilter& filter)
{
    filter_ = filter;
    ListView_SetItemState(list_, -1, 0, LVIS_SELECTED);
    Reindex();
    if (callbacks_.selectionChanged) {
        callbacks_.selectionChanged();
    }
}

void DownloadListView::SetSort(const SortSpec& sort)
{
    sort_ = sort;
    UpdateSortArrow();
    if (snapshot_) {
        SetSnapshot(snapshot_);  // re-sorts and keeps the selection
    }
}

void DownloadListView::ApplyTheme(const Theme& theme)
{
    theme.ApplyToControl(list_);
    theme.ApplyToControl(ListView_GetHeader(list_));
    ListView_SetBkColor(list_, theme.Colors().window);
    ListView_SetTextBkColor(list_, theme.Colors().window);
    ListView_SetTextColor(list_, theme.Colors().text);
    // Grid lines; the list control draws them in a light system color that glares in dark mode.
    ListView_SetExtendedListViewStyleEx(list_, LVS_EX_GRIDLINES, theme.IsDark() ? 0 : LVS_EX_GRIDLINES);
    const int mark = Scale(kQueueMark96, DpiOf(list_));
    queueIcon_ = CreateGlyphIcon(Glyph{0xE8FD, RGB(0x1E, 0x88, 0xE5)}, mark);      // BulletedList
    syncQueueIcon_ = CreateGlyphIcon(Glyph{0xE895, RGB(0x2E, 0xA0, 0x43)}, mark);  // Sync
    ::InvalidateRect(list_, nullptr, TRUE);
}

void DownloadListView::SetFont(HFONT font)
{
    ::SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

std::vector<const core::DownloadView*> DownloadListView::Selection() const
{
    std::vector<const core::DownloadView*> views;
    if (!snapshot_) {
        return views;
    }
    for (int row = ListView_GetNextItem(list_, -1, LVNI_SELECTED); row >= 0;
         row = ListView_GetNextItem(list_, row, LVNI_SELECTED)) {
        if (static_cast<size_t>(row) < index_.size()) {
            views.push_back(snapshot_->downloads[index_[static_cast<size_t>(row)]].get());
        }
    }
    return views;
}

std::vector<model::DownloadId> DownloadListView::SelectedIds() const
{
    std::vector<model::DownloadId> ids;
    for (const core::DownloadView* view : Selection()) {
        ids.push_back(view->download.id);
    }
    return ids;
}

void DownloadListView::SelectAll()
{
    ListView_SetItemState(list_, -1, LVIS_SELECTED, LVIS_SELECTED);
}

void DownloadListView::SelectOnly(model::DownloadId id)
{
    if (!snapshot_) {
        return;
    }
    for (size_t row = 0; row < index_.size(); ++row) {
        if (snapshot_->downloads[index_[row]]->download.id == id) {
            ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_SetItemState(list_, static_cast<int>(row), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(list_, static_cast<int>(row), FALSE);
            return;
        }
    }
}

std::optional<size_t> DownloadListView::FocusedPosition() const
{
    const int row = ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
    if (row < 0 || static_cast<size_t>(row) >= index_.size()) {
        return std::nullopt;
    }
    return static_cast<size_t>(row);
}

bool DownloadListView::Find(const FindOptions& options, bool fromStart)
{
    if (!snapshot_) {
        return false;
    }
    const auto found = FindNext(*snapshot_, index_, fromStart ? std::nullopt : FocusedPosition(), options);
    if (!found) {
        return false;
    }
    SelectOnly(snapshot_->downloads[index_[*found]]->download.id);
    return true;
}

std::wstring DownloadListView::CellText(const core::DownloadView& view, Column column) const
{
    const model::Download& d = view.download;
    switch (column) {
    case Column::Queue:
        return {};
    case Column::FileName:
        return base::ToUtf16(d.fileName);
    case Column::Size:
        return FormatSizeColumn(view);
    case Column::Status:
        return FormatStatus(view);
    case Column::TimeLeft:
        return d.status == model::DownloadStatus::Downloading ? FormatTimeLeft(view.secondsLeft) : std::wstring();
    case Column::TransferRate:
        return d.status == model::DownloadStatus::Downloading ? FormatSpeed(view.bytesPerSecond) : std::wstring();
    case Column::LastTryDate:
        return FormatDateTime(d.lastTryAtMs);
    case Column::Description:
        return base::ToUtf16(d.description);
    case Column::DateAdded:
        return FormatDateTime(d.createdAtMs);
    case Column::SaveTo:
        return base::ToUtf16(d.saveDir);
    case Column::Referer:
        return base::ToUtf16(d.referer.value_or(""));
    case Column::ParentWebPage:
        return base::ToUtf16(d.parentPage.value_or(""));
    }
    return {};
}

const model::Queue* DownloadListView::QueueOf(const core::DownloadView& view) const
{
    if (!snapshot_ || !view.download.queueId) {
        return nullptr;
    }
    for (const core::QueueView& queue : snapshot_->queues) {
        if (queue.queue.id == *view.download.queueId) {
            return &queue.queue;
        }
    }
    return nullptr;
}

void DownloadListView::DrawQueueMark(const NMLVCUSTOMDRAW& draw) const
{
    const auto row = static_cast<size_t>(draw.nmcd.dwItemSpec);  // an owner-data list numbers rows
    if (!snapshot_ || row >= index_.size()) {
        return;
    }
    const model::Queue* queue = QueueOf(*snapshot_->downloads[index_[row]]);
    if (queue == nullptr) {
        return;
    }
    RECT cell{};
    // The first column's bounds are the whole row; its label is the cell.
    if (!ListView_GetSubItemRect(list_, static_cast<int>(row), draw.iSubItem, draw.iSubItem == 0 ? LVIR_LABEL : LVIR_BOUNDS,
                                 &cell)) {
        return;
    }
    const int size = Scale(kQueueMark96, DpiOf(list_));
    const int x = static_cast<int>(cell.left) + std::max(0, static_cast<int>(cell.right - cell.left - size) / 2);
    const int y = static_cast<int>(cell.top) + std::max(0, static_cast<int>(cell.bottom - cell.top - size) / 2);
    const HICON icon = queue->kind == model::QueueKind::Sync ? syncQueueIcon_.get() : queueIcon_.get();
    ::DrawIconEx(draw.nmcd.hdc, x, y, icon, size, size, 0, nullptr, DI_NORMAL);
}

int DownloadListView::IconFor(const core::DownloadView& view)
{
    const std::wstring extension = base::ToUtf16(core::FileExtension(view.download.fileName));
    const auto cached = iconCache_.find(extension);
    if (cached != iconCache_.end()) {
        return cached->second;
    }
    SHFILEINFOW info{};
    const std::wstring probe = L"file." + (extension.empty() ? std::wstring(L"bin") : extension);
    ::SHGetFileInfoW(probe.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                     SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    iconCache_.emplace(extension, info.iIcon);
    return info.iIcon;
}

bool DownloadListView::HandleNotify(const NMHDR& header, LRESULT& result)
{
    const HWND listHeader = ListView_GetHeader(list_);
    if (header.hwndFrom == listHeader) {
        if (header.code == HDN_ITEMCHANGEDW && !fitting_) {
            // A width the user dragged becomes the chosen width; then the last column refits.
            const auto& change = reinterpret_cast<const NMHEADERW&>(header);
            if (change.pitem != nullptr && (change.pitem->mask & HDI_WIDTH) != 0 && change.iItem >= 0 &&
                change.iItem < static_cast<int>(shown_.size())) {
                chosenWidths96_[shown_[static_cast<size_t>(change.iItem)]] = Unscale(change.pitem->cxy, DpiOf(list_));
                FitLastColumn();
            }
        }
        if (header.code == HDN_ENDTRACKW || header.code == HDN_ENDDRAG) {
            if (callbacks_.layoutChanged) {
                callbacks_.layoutChanged();
            }
        }
        return false;  // the list view still needs these
    }
    if (header.hwndFrom != list_) {
        return false;
    }
    switch (header.code) {
    case LVN_GETDISPINFOW: {
        auto& info = const_cast<NMLVDISPINFOW&>(reinterpret_cast<const NMLVDISPINFOW&>(header));  // in-out by contract
        LVITEMW& item = info.item;
        if (!snapshot_ || item.iItem < 0 || static_cast<size_t>(item.iItem) >= index_.size() || item.iSubItem < 0 ||
            static_cast<size_t>(item.iSubItem) >= shown_.size()) {
            result = 0;
            return true;
        }
        const core::DownloadView& view = *snapshot_->downloads[index_[static_cast<size_t>(item.iItem)]];
        const Column column = shown_[static_cast<size_t>(item.iSubItem)];
        if ((item.mask & LVIF_TEXT) != 0 && item.pszText != nullptr && item.cchTextMax > 0) {
            const std::wstring text = CellText(view, column);
            wcsncpy_s(item.pszText, static_cast<size_t>(item.cchTextMax), text.c_str(), _TRUNCATE);
        }
        if ((item.mask & LVIF_IMAGE) != 0) {
            item.iImage = column == Column::FileName ? IconFor(view) : I_IMAGENONE;
        }
        result = 0;
        return true;
    }
    case NM_CUSTOMDRAW: {
        const auto& draw = reinterpret_cast<const NMLVCUSTOMDRAW&>(header);
        const auto isQueue = [this](int subItem) {
            return subItem >= 0 && static_cast<size_t>(subItem) < shown_.size() &&
                   shown_[static_cast<size_t>(subItem)] == Column::Queue;
        };
        switch (draw.nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            result = CDRF_NOTIFYITEMDRAW;
            break;
        case CDDS_ITEMPREPAINT:
            result = CDRF_NOTIFYSUBITEMDRAW;
            break;
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
            result = isQueue(draw.iSubItem) ? CDRF_NOTIFYPOSTPAINT : CDRF_DODEFAULT;
            break;
        case CDDS_ITEMPOSTPAINT | CDDS_SUBITEM:
            if (isQueue(draw.iSubItem)) {
                DrawQueueMark(draw);
            }
            result = CDRF_DODEFAULT;
            break;
        default:
            result = CDRF_DODEFAULT;
            break;
        }
        return true;
    }
    case LVN_ITEMCHANGED:
    case LVN_ODSTATECHANGED:
        if (!updating_ && callbacks_.selectionChanged) {
            callbacks_.selectionChanged();
        }
        result = 0;
        return true;
    case LVN_COLUMNCLICK: {
        const auto& info = reinterpret_cast<const NMLISTVIEW&>(header);
        if (info.iSubItem >= 0 && static_cast<size_t>(info.iSubItem) < shown_.size()) {
            if (const auto key = SortKeyForColumn(shown_[static_cast<size_t>(info.iSubItem)])) {
                SetSort(SortSpec{*key, sort_.key == *key ? !sort_.descending : false});
                if (callbacks_.layoutChanged) {
                    callbacks_.layoutChanged();
                }
            }
        }
        result = 0;
        return true;
    }
    case NM_DBLCLK: {
        const auto& info = reinterpret_cast<const NMITEMACTIVATE&>(header);
        if (info.iItem >= 0 && callbacks_.activated) {
            callbacks_.activated();
        }
        result = 0;
        return true;
    }
    case NM_RCLICK: {
        POINT screen{};
        ::GetCursorPos(&screen);
        if (callbacks_.contextMenu) {
            callbacks_.contextMenu(screen);
        }
        result = 0;
        return true;
    }
    default:
        return false;
    }
}

}  // namespace ldm::ui
