#pragma once

#include "core/Snapshot.h"
#include "model/Queue.h"
#include "ui/Handles.h"
#include "ui/ListModel.h"
#include "ui/Theme.h"

#include <windows.h>
#include <commctrl.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ldm::ui {

// The download list (checklist L-01 to L-04): a virtual report list view over the current
// snapshot (design section 6), so only visible rows are formatted. Selection follows downloads by
// id across snapshots, sorting and filtering.
class DownloadListView {
public:
    struct Callbacks {
        std::function<void()> selectionChanged;
        std::function<void()> activated;  // double-click or Enter on a row
        std::function<void(POINT)> contextMenu;
        std::function<void()> layoutChanged;  // columns or sort changed; persist them
    };

    DownloadListView() = default;
    DownloadListView(const DownloadListView&) = delete;
    DownloadListView& operator=(const DownloadListView&) = delete;

    void Create(HWND parent, Callbacks callbacks);
    [[nodiscard]] HWND Handle() const noexcept { return list_; }

    void SetSnapshot(std::shared_ptr<const core::Snapshot> snapshot);
    void SetFilter(const ListFilter& filter);
    void SetSort(const SortSpec& sort);
    [[nodiscard]] const SortSpec& Sort() const noexcept { return sort_; }
    void SetColumns(const std::vector<ColumnState>& columns);
    [[nodiscard]] std::vector<ColumnState> Columns() const;
    void ApplyTheme(const Theme& theme);
    void SetFont(HFONT font);
    // Stretches the last column over the space to its right, so no empty, column-like box shows
    // after it. Call after the list is resized.
    void FitLastColumn();

    [[nodiscard]] std::vector<const core::DownloadView*> Selection() const;
    [[nodiscard]] std::vector<model::DownloadId> SelectedIds() const;
    void SelectAll();
    void SelectOnly(model::DownloadId id);

    // Find and Find Next (checklist L-06); returns false when nothing matches.
    bool Find(const FindOptions& options, bool fromStart);

    bool HandleNotify(const NMHDR& header, LRESULT& result);

private:
    void Reindex();
    void ApplyColumns();
    void UpdateSortArrow();
    [[nodiscard]] std::wstring CellText(const core::DownloadView& view, Column column) const;
    [[nodiscard]] int IconFor(const core::DownloadView& view);
    [[nodiscard]] std::optional<size_t> FocusedPosition() const;
    // The Q column (checklist L-01): a mark on members of a download queue, drawn over the empty
    // cell, with the queue's name as the tip.
    void DrawQueueMark(const NMLVCUSTOMDRAW& draw) const;
    [[nodiscard]] const model::Queue* QueueOf(const core::DownloadView& view) const;
    // The tip over the Q column: "Queue: <name>", or nothing elsewhere. The list view asks for
    // no tip over an empty cell, so the list has a tooltip of its own, fed from mouse moves.
    [[nodiscard]] std::wstring QueueTipAt(POINT client) const;
    static LRESULT CALLBACK ListSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                         DWORD_PTR data);

    Callbacks callbacks_;
    HWND list_ = nullptr;
    std::shared_ptr<const core::Snapshot> snapshot_;
    ListFilter filter_;
    SortSpec sort_;
    std::vector<size_t> index_;  // list row -> position in snapshot_->downloads
    std::vector<ColumnState> columns_;  // every column, in display order of the visible ones
    std::vector<Column> shown_;         // list view column -> column id
    // Widths the user chose, at 96 DPI. The last column may be shown wider than this to fill the
    // list; Columns() reports these so the stretch is never saved.
    std::map<Column, int> chosenWidths96_;
    bool fitting_ = false;
    std::map<std::wstring, int> iconCache_;  // extension -> system image list index
    UniqueIcon queueIcon_;
    UniqueIcon syncQueueIcon_;
    HWND queueTip_ = nullptr;  // owned by the list, which destroys it
    std::wstring queueTipText_;
    bool updating_ = false;
};

}  // namespace ldm::ui
