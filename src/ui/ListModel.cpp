#include "ui/ListModel.h"

#include "base/Utf.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <compare>
#include <set>

namespace ldm::ui {

namespace {

struct ColumnInfo {
    Column id;
    std::string_view key;
    std::wstring_view title;
    int width;
    bool visible;
    std::optional<SortKey> sortKey;
};

constexpr std::array<ColumnInfo, kColumnCount> kColumns{{
    // Default widths at 100% scaling; Last Try Date is wider so the date stays readable.
    {Column::Queue, "queue", L"Q", 20, true, std::nullopt},
    {Column::FileName, "name", L"File Name", 152, true, SortKey::FileName},
    {Column::Size, "size", L"Size", 75, true, SortKey::Size},
    {Column::Status, "status", L"Status", 65, true, SortKey::Status},
    {Column::TimeLeft, "left", L"Time left", 75, true, SortKey::TimeLeft},
    {Column::TransferRate, "rate", L"Transfer rate", 94, true, SortKey::TransferRate},
    {Column::LastTryDate, "lasttry", L"Last Try Date", 100, true, SortKey::LastTryDate},
    {Column::Description, "description", L"Description", 150, true, SortKey::Description},
    {Column::DateAdded, "added", L"Date Added", 130, false, SortKey::DateAdded},
    {Column::SaveTo, "saveto", L"Save To", 200, false, SortKey::SavePath},
    {Column::Referer, "referer", L"Referer", 200, false, SortKey::Referer},
    {Column::ParentWebPage, "parent", L"Parent Web Page", 200, false, SortKey::ParentWebPage},
}};

constexpr std::array<std::pair<SortKey, std::string_view>, 12> kSortNames{{
    {SortKey::OrderOfAddition, "order"},
    {SortKey::FileName, "name"},
    {SortKey::Size, "size"},
    {SortKey::Status, "status"},
    {SortKey::TimeLeft, "left"},
    {SortKey::TransferRate, "rate"},
    {SortKey::LastTryDate, "lasttry"},
    {SortKey::Description, "description"},
    {SortKey::SavePath, "saveto"},
    {SortKey::Referer, "referer"},
    {SortKey::ParentWebPage, "parent"},
    {SortKey::DateAdded, "added"},
}};

const ColumnInfo& Info(Column column)
{
    return kColumns[static_cast<size_t>(column)];
}

// Locale-aware, case-insensitive comparison of UTF-8 text, as Explorer sorts names.
int CompareText(const std::string& a, const std::string& b)
{
    if (a == b) {
        return 0;
    }
    const std::wstring wa = base::ToUtf16(a);
    const std::wstring wb = base::ToUtf16(b);
    const int result = ::CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, wa.c_str(),
                                         static_cast<int>(wa.size()), wb.c_str(), static_cast<int>(wb.size()), nullptr,
                                         nullptr, 0);
    return result == 0 ? 0 : result - CSTR_EQUAL;
}

template <typename T>
int CompareOptional(const std::optional<T>& a, const std::optional<T>& b)
{
    if (a.has_value() != b.has_value()) {
        return a.has_value() ? 1 : -1;  // unknown first
    }
    if (!a) {
        return 0;
    }
    return *a < *b ? -1 : (*b < *a ? 1 : 0);
}

int StatusRank(const core::DownloadView& view)
{
    // Active first, then waiting, then finished states, as a user scanning the column expects.
    switch (view.download.status) {
    case model::DownloadStatus::Downloading:
        return 0;
    case model::DownloadStatus::Stopping:
    case model::DownloadStatus::Finalizing:
        return 1;
    case model::DownloadStatus::Queued:
        return 2;
    case model::DownloadStatus::Paused:
        return 3;
    case model::DownloadStatus::Failed:
        return 4;
    case model::DownloadStatus::Completed:
        return 5;
    case model::DownloadStatus::Deleting:
        return 6;
    }
    return 7;
}

int Compare(const core::DownloadView& a, const core::DownloadView& b, SortKey key)
{
    const model::Download& x = a.download;
    const model::Download& y = b.download;
    switch (key) {
    case SortKey::OrderOfAddition:
        return 0;
    case SortKey::FileName:
        return CompareText(x.fileName, y.fileName);
    case SortKey::Size:
        return CompareOptional(x.totalSize, y.totalSize);
    case SortKey::Status: {
        const int rank = StatusRank(a) - StatusRank(b);
        if (rank != 0) {
            return rank;
        }
        return CompareOptional(x.totalSize ? std::optional<std::int64_t>(a.receivedBytes) : std::nullopt,
                               y.totalSize ? std::optional<std::int64_t>(b.receivedBytes) : std::nullopt);
    }
    case SortKey::TimeLeft:
        return CompareOptional(a.secondsLeft, b.secondsLeft);
    case SortKey::TransferRate:
        return a.bytesPerSecond < b.bytesPerSecond ? -1 : (a.bytesPerSecond > b.bytesPerSecond ? 1 : 0);
    case SortKey::LastTryDate:
        return CompareOptional(x.lastTryAtMs, y.lastTryAtMs);
    case SortKey::Description:
        return CompareText(x.description, y.description);
    case SortKey::SavePath:
        return CompareText(x.saveDir, y.saveDir);
    case SortKey::Referer:
        return CompareText(x.referer.value_or(""), y.referer.value_or(""));
    case SortKey::ParentWebPage:
        return CompareText(x.parentPage.value_or(""), y.parentPage.value_or(""));
    case SortKey::DateAdded:
        return x.createdAtMs < y.createdAtMs ? -1 : (x.createdAtMs > y.createdAtMs ? 1 : 0);
    }
    return 0;
}

bool Matches(const core::DownloadView& view, const ListFilter& filter)
{
    const bool finished = view.download.status == model::DownloadStatus::Completed;
    if (filter.status == StatusFilter::Unfinished && finished) {
        return false;
    }
    if (filter.status == StatusFilter::Finished && !finished) {
        return false;
    }
    return !filter.category || view.download.categoryId == filter.category;
}

std::wstring Fold(std::wstring_view text, bool matchCase)
{
    std::wstring folded(text);
    if (!matchCase && !folded.empty()) {
        ::CharLowerBuffW(folded.data(), static_cast<DWORD>(folded.size()));
    }
    return folded;
}

bool FieldMatches(const std::string& field, const std::wstring& needle, const FindOptions& options)
{
    const std::wstring text = Fold(base::ToUtf16(field), options.matchCase);
    return options.wholeString ? text == needle : text.find(needle) != std::wstring::npos;
}

}  // namespace

std::string SerializeSort(const SortSpec& sort)
{
    for (const auto& [key, name] : kSortNames) {
        if (key == sort.key) {
            return std::string(name) + (sort.descending ? ":desc" : ":asc");
        }
    }
    return "order:asc";
}

SortSpec ParseSort(std::string_view text)
{
    SortSpec sort;
    const size_t colon = text.find(':');
    const std::string_view name = text.substr(0, colon);
    for (const auto& [key, keyName] : kSortNames) {
        if (keyName == name) {
            sort.key = key;
            sort.descending = colon != std::string_view::npos && text.substr(colon + 1) == "desc";
            return sort;
        }
    }
    return SortSpec{};
}

std::vector<size_t> BuildIndex(const core::Snapshot& snapshot, const ListFilter& filter, const SortSpec& sort)
{
    std::vector<size_t> index;
    index.reserve(snapshot.downloads.size());
    for (size_t i = 0; i < snapshot.downloads.size(); ++i) {
        if (Matches(*snapshot.downloads[i], filter)) {
            index.push_back(i);
        }
    }
    // Snapshot order is the order of addition, so a stable sort keeps it for ties.
    std::stable_sort(index.begin(), index.end(), [&](size_t a, size_t b) {
        const int order = Compare(*snapshot.downloads[a], *snapshot.downloads[b], sort.key);
        return sort.descending ? order > 0 : order < 0;
    });
    if (sort.key == SortKey::OrderOfAddition && sort.descending) {
        std::reverse(index.begin(), index.end());
    }
    return index;
}

std::wstring_view ColumnTitle(Column column)
{
    return Info(column).title;
}

std::optional<SortKey> SortKeyForColumn(Column column)
{
    return Info(column).sortKey;
}

std::vector<ColumnState> DefaultColumns()
{
    // File Name shows first, then the queue marker; kColumns stays in Column order.
    std::vector<ColumnState> columns;
    for (const ColumnInfo& info : kColumns) {
        columns.push_back(ColumnState{info.id, info.width, info.visible});
    }
    std::swap(columns[static_cast<size_t>(Column::Queue)], columns[static_cast<size_t>(Column::FileName)]);
    return columns;
}

std::string SerializeColumns(const std::vector<ColumnState>& columns)
{
    std::string text;
    for (const ColumnState& column : columns) {
        if (!text.empty()) {
            text += ',';
        }
        text += std::string(Info(column.id).key) + ":" + std::to_string(column.width) + ":" + (column.visible ? "1" : "0");
    }
    return text;
}

std::vector<ColumnState> ParseColumns(std::string_view text)
{
    std::vector<ColumnState> columns;
    std::set<Column> seen;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(',', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view entry = text.substr(pos, end - pos);
        pos = end + 1;
        const size_t first = entry.find(':');
        const size_t second = entry.find(':', first == std::string_view::npos ? first : first + 1);
        if (first == std::string_view::npos || second == std::string_view::npos) {
            continue;
        }
        const std::string_view key = entry.substr(0, first);
        const std::string_view widthText = entry.substr(first + 1, second - first - 1);
        const std::string_view visibleText = entry.substr(second + 1);
        const auto info = std::find_if(kColumns.begin(), kColumns.end(), [&](const ColumnInfo& c) { return c.key == key; });
        int width = 0;
        const auto [ptr, error] = std::from_chars(widthText.data(), widthText.data() + widthText.size(), width);
        if (info == kColumns.end() || error != std::errc{} || ptr != widthText.data() + widthText.size() ||
            (visibleText != "0" && visibleText != "1") || seen.contains(info->id)) {
            continue;
        }
        seen.insert(info->id);
        columns.push_back(ColumnState{info->id, std::clamp(width, 16, 2000), visibleText == "1"});
    }
    if (columns.empty() || std::none_of(columns.begin(), columns.end(), [](const ColumnState& c) { return c.visible; })) {
        return DefaultColumns();
    }
    for (const ColumnInfo& info : kColumns) {
        if (!seen.contains(info.id)) {
            columns.push_back(ColumnState{info.id, info.width, false});
        }
    }
    return columns;
}

std::optional<size_t> FindNext(const core::Snapshot& snapshot, const std::vector<size_t>& index,
                               std::optional<size_t> after, const FindOptions& options)
{
    if (options.text.empty() || index.empty()) {
        return std::nullopt;
    }
    const std::wstring needle = Fold(options.text, options.matchCase);
    const size_t start = after ? (*after + 1) % index.size() : 0;
    for (size_t step = 0; step < index.size(); ++step) {
        const size_t position = (start + step) % index.size();
        const model::Download& d = snapshot.downloads[index[position]]->download;
        const bool found = (options.inFileName && FieldMatches(d.fileName, needle, options)) ||
                           (options.inDescription && FieldMatches(d.description, needle, options)) ||
                           (options.inAddresses &&
                            (FieldMatches(d.url, needle, options) || FieldMatches(d.referer.value_or(""), needle, options) ||
                             FieldMatches(d.parentPage.value_or(""), needle, options)));
        if (found) {
            return position;
        }
    }
    return std::nullopt;
}

}  // namespace ldm::ui
