#include "ui/DownloadListView.h"

#include "base/Utf.h"
#include "core/FileNames.h"
#include "ui/Dpi.h"
#include "ui/Format.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <set>

namespace ldm::ui {

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
    const UINT dpi = DpiOf(list_);
    for (const ColumnState& column : columns_) {
        if (!column.visible) {
            continue;
        }
        std::wstring title(ColumnTitle(column.id));
        LVCOLUMNW data{};
        data.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        data.fmt = LVCFMT_LEFT;  // the reference aligns every column, sizes and rates included, to the left
        data.cx = Scale(column.width, dpi);
        data.pszText = title.data();
        ListView_InsertColumn(list_, static_cast<int>(shown_.size()), &data);
        shown_.push_back(column.id);
    }
    UpdateSortArrow();
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
        result.push_back(ColumnState{id, Unscale(ListView_GetColumnWidth(list_, position), dpi), true});
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
    // the reference's grid lines; the list control draws them in a light system color that glares in dark mode.
    ListView_SetExtendedListViewStyleEx(list_, LVS_EX_GRIDLINES, theme.IsDark() ? 0 : LVS_EX_GRIDLINES);
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
