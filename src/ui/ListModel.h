#pragma once

#include "core/Snapshot.h"
#include "model/Ids.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Filtering, sorting, searching and column layout of the download list (checklist L-01 to L-03,
// L-06, W-02). Pure functions over a snapshot; the list view asks for rows through the index.
namespace ldm::ui {

enum class StatusFilter {
    All,
    Unfinished,
    Finished,
};

// A node of the category tree: a status view, optionally narrowed to one category.
struct ListFilter {
    StatusFilter status = StatusFilter::All;
    std::optional<model::CategoryId> category;

    auto operator<=>(const ListFilter&) const = default;
};

// View > Arrange files (checklist MN-04, L-03).
enum class SortKey {
    OrderOfAddition,
    FileName,
    Size,
    Status,
    TimeLeft,
    TransferRate,
    LastTryDate,
    Description,
    SavePath,
    Referer,
    ParentWebPage,
    DateAdded,
};

struct SortSpec {
    SortKey key = SortKey::OrderOfAddition;
    bool descending = false;
};

[[nodiscard]] std::string SerializeSort(const SortSpec& sort);
[[nodiscard]] SortSpec ParseSort(std::string_view text);

// Positions into snapshot.downloads, filtered and in display order. Unknown sizes sort before
// every known size; ties keep the order of addition.
[[nodiscard]] std::vector<size_t> BuildIndex(const core::Snapshot& snapshot, const ListFilter& filter,
                                             const SortSpec& sort);

enum class Column {
    Queue,
    FileName,
    Size,
    Status,
    TimeLeft,
    TransferRate,
    LastTryDate,
    Description,
    DateAdded,
    SaveTo,
    Referer,
    ParentWebPage,
};

inline constexpr size_t kColumnCount = 12;

struct ColumnState {
    Column id = Column::FileName;
    int width = 100;  // at 96 DPI
    bool visible = true;
};

[[nodiscard]] std::wstring_view ColumnTitle(Column column);
// The sort key a click on this column's header selects, if any.
[[nodiscard]] std::optional<SortKey> SortKeyForColumn(Column column);
// Every column in its default order, with default widths and visibility.
[[nodiscard]] std::vector<ColumnState> DefaultColumns();
// "id:width:visible" entries joined by ','; the stored form of View > Customize list columns.
[[nodiscard]] std::string SerializeColumns(const std::vector<ColumnState>& columns);
// Restores a stored layout. Unknown or repeated entries are dropped; columns missing from the
// text are appended hidden; an unusable text yields the default layout.
[[nodiscard]] std::vector<ColumnState> ParseColumns(std::string_view text);

// Find dialog (checklist L-06).
struct FindOptions {
    std::wstring text;
    bool inFileName = true;
    bool inDescription = true;
    bool inAddresses = false;  // address, referer and parent web page
    bool matchCase = false;
    bool wholeString = false;
};

// The first matching position in `index` after `after` (or from the start when absent), wrapping
// around once. Nothing when no row matches.
[[nodiscard]] std::optional<size_t> FindNext(const core::Snapshot& snapshot, const std::vector<size_t>& index,
                                             std::optional<size_t> after, const FindOptions& options);

}  // namespace ldm::ui
