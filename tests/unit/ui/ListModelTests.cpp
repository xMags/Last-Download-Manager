#include "ui/Format.h"
#include "ui/ListModel.h"

#include <gtest/gtest.h>

#include <memory>

namespace {

using ldm::core::DownloadView;
using ldm::core::Snapshot;
using ldm::model::CategoryId;
using ldm::model::DownloadStatus;
using ldm::ui::BuildIndex;
using ldm::ui::Column;
using ldm::ui::DefaultColumns;
using ldm::ui::FindNext;
using ldm::ui::FindOptions;
using ldm::ui::FormatSize;
using ldm::ui::FormatSizeColumn;
using ldm::ui::FormatSpeed;
using ldm::ui::FormatStatus;
using ldm::ui::FormatTimeLeft;
using ldm::ui::ParseColumns;
using ldm::ui::ParseSort;
using ldm::ui::SerializeColumns;
using ldm::ui::SerializeSort;
using ldm::ui::SortKey;
using ldm::ui::SortSpec;
using ldm::ui::StatusFilter;
using ldm::ui::kColumnCount;

std::shared_ptr<const DownloadView> Row(std::int64_t id, std::string name, DownloadStatus status,
                                        std::optional<std::int64_t> size, std::int64_t received = 0,
                                        std::optional<CategoryId> category = std::nullopt)
{
    auto view = std::make_shared<DownloadView>();
    view->download.id = ldm::model::DownloadId{id};
    view->download.fileName = std::move(name);
    view->download.status = status;
    view->download.totalSize = size;
    view->download.categoryId = category;
    view->download.createdAtMs = id;
    view->receivedBytes = received;
    return view;
}

Snapshot Sample()
{
    Snapshot snapshot;
    snapshot.downloads = {
        Row(1, "beta.zip", DownloadStatus::Completed, 3000, 3000, CategoryId{2}),
        Row(2, "Alpha.iso", DownloadStatus::Downloading, 10'000, 2500, CategoryId{5}),
        Row(3, "file10.bin", DownloadStatus::Paused, std::nullopt, 100, CategoryId{5}),
        Row(4, "file9.bin", DownloadStatus::Failed, 500, 0),
    };
    return snapshot;
}

std::vector<std::int64_t> Ids(const Snapshot& snapshot, const std::vector<size_t>& index)
{
    std::vector<std::int64_t> ids;
    for (const size_t i : index) {
        ids.push_back(snapshot.downloads[i]->download.id.value);
    }
    return ids;
}

TEST(Format, Sizes)
{
    EXPECT_EQ(FormatSize(0), L"0 Bytes");
    EXPECT_EQ(FormatSize(1023), L"1023 Bytes");
    EXPECT_EQ(FormatSize(1536), L"1.50 KB");
    EXPECT_EQ(FormatSize(1024 * 1024 - 1), L"1023.99 KB");
    EXPECT_EQ(FormatSize(12'939'427), L"12.33 MB");
    EXPECT_EQ(FormatSize(5LL * 1024 * 1024 * 1024), L"5.00 GB");
    EXPECT_EQ(FormatSpeed(0), L"");
    EXPECT_EQ(FormatSpeed(2048), L"2.00 KB/sec");
}

TEST(Format, TimeLeft)
{
    EXPECT_EQ(FormatTimeLeft(std::nullopt), L"");
    EXPECT_EQ(FormatTimeLeft(45), L"45 sec");
    EXPECT_EQ(FormatTimeLeft(185), L"3 min 05 sec");
    EXPECT_EQ(FormatTimeLeft(7800), L"2 hour(s) 10 min");
    EXPECT_EQ(FormatTimeLeft(273600), L"3 day(s) 4 hour(s)");
}

TEST(Format, StatusText)
{
    const Snapshot snapshot = Sample();
    EXPECT_EQ(FormatStatus(*snapshot.downloads[0]), L"Complete");
    EXPECT_EQ(FormatStatus(*snapshot.downloads[1]), L"25.00%");
    EXPECT_EQ(FormatStatus(*snapshot.downloads[2]), L"Paused");
    EXPECT_EQ(FormatStatus(*snapshot.downloads[3]), L"Error");
    auto nearlyDone = Row(9, "x", DownloadStatus::Downloading, 100'000, 99'999);
    EXPECT_EQ(FormatStatus(*nearlyDone), L"99.99%") << "never 100% before completion";
    auto unknown = Row(10, "x", DownloadStatus::Downloading, std::nullopt, 0);
    EXPECT_EQ(FormatStatus(*unknown), L"Connecting...");
    EXPECT_EQ(FormatSizeColumn(*snapshot.downloads[2]), L"100 Bytes");
}

TEST(ListModel, FiltersByStatusAndCategory)
{
    const Snapshot snapshot = Sample();
    EXPECT_EQ(Ids(snapshot, BuildIndex(snapshot, {}, {})), (std::vector<std::int64_t>{1, 2, 3, 4}));
    EXPECT_EQ(Ids(snapshot, BuildIndex(snapshot, {StatusFilter::Unfinished, std::nullopt}, {})),
              (std::vector<std::int64_t>{2, 3, 4}));
    EXPECT_EQ(Ids(snapshot, BuildIndex(snapshot, {StatusFilter::Finished, std::nullopt}, {})), (std::vector<std::int64_t>{1}));
    EXPECT_EQ(Ids(snapshot, BuildIndex(snapshot, {StatusFilter::All, CategoryId{5}}, {})), (std::vector<std::int64_t>{2, 3}));
    EXPECT_TRUE(BuildIndex(snapshot, {StatusFilter::Finished, CategoryId{5}}, {}).empty());
}

TEST(ListModel, HidesGrabberDownloadsNotInTheMainList)
{
    Snapshot snapshot = Sample();
    auto hidden = std::make_shared<DownloadView>(*snapshot.downloads[0]);
    hidden->download.id = ldm::model::DownloadId{9};
    hidden->download.projectId = ldm::model::GrabberProjectId{1};
    hidden->download.listed = false;
    auto orphan = std::make_shared<DownloadView>(*hidden);
    orphan->download.id = ldm::model::DownloadId{10};
    orphan->download.projectId.reset();  // its project was deleted: shown again
    auto added = std::make_shared<DownloadView>(*hidden);
    added->download.id = ldm::model::DownloadId{11};
    added->download.listed = true;
    snapshot.downloads.push_back(hidden);
    snapshot.downloads.push_back(orphan);
    snapshot.downloads.push_back(added);
    EXPECT_EQ(Ids(snapshot, BuildIndex(snapshot, {}, {})), (std::vector<std::int64_t>{1, 2, 3, 4, 10, 11}));
}

TEST(ListModel, QueueNodesListTheMembersInQueueOrder)
{
    Snapshot snapshot = Sample();
    auto first = std::make_shared<DownloadView>(*snapshot.downloads[3]);
    first->download.queueId = ldm::model::QueueId{7};
    first->download.queuePosition = 1;
    auto second = std::make_shared<DownloadView>(*snapshot.downloads[0]);
    second->download.queueId = ldm::model::QueueId{7};
    second->download.queuePosition = 2;
    auto other = std::make_shared<DownloadView>(*snapshot.downloads[2]);
    other->download.queueId = ldm::model::QueueId{9};
    snapshot.downloads[3] = first;
    snapshot.downloads[0] = second;
    snapshot.downloads[2] = other;

    const ldm::ui::ListFilter queue{StatusFilter::Queued, std::nullopt, ldm::model::QueueId{7}};
    EXPECT_EQ(Ids(snapshot, BuildIndex(snapshot, queue, {})), (std::vector<std::int64_t>{4, 1})) << "queue order";
    EXPECT_EQ(Ids(snapshot, BuildIndex(snapshot, queue, {SortKey::FileName, false})), (std::vector<std::int64_t>{1, 4}))
        << "another sort still applies";
    EXPECT_EQ(Ids(snapshot, BuildIndex(snapshot, {StatusFilter::Queued, std::nullopt, std::nullopt}, {})),
              (std::vector<std::int64_t>{1, 3, 4})) << "the Queues node lists every member";

    auto waiting = std::make_shared<DownloadView>(*first);
    waiting->waitingInQueue = true;
    EXPECT_EQ(FormatStatus(*waiting), L"Waiting in queue");
}

TEST(ListModel, SortsNamesNaturallyAndUnknownSizesFirst)
{
    const Snapshot snapshot = Sample();
    EXPECT_EQ(Ids(snapshot, BuildIndex(snapshot, {}, {SortKey::FileName, false})), (std::vector<std::int64_t>{2, 1, 4, 3}));
    EXPECT_EQ(Ids(snapshot, BuildIndex(snapshot, {}, {SortKey::Size, false})), (std::vector<std::int64_t>{3, 4, 1, 2}));
    EXPECT_EQ(Ids(snapshot, BuildIndex(snapshot, {}, {SortKey::Size, true})), (std::vector<std::int64_t>{2, 1, 4, 3}));
    EXPECT_EQ(Ids(snapshot, BuildIndex(snapshot, {}, {SortKey::Status, false})), (std::vector<std::int64_t>{2, 3, 4, 1}));
    EXPECT_EQ(Ids(snapshot, BuildIndex(snapshot, {}, {SortKey::OrderOfAddition, true})), (std::vector<std::int64_t>{4, 3, 2, 1}));
}

TEST(ListModel, SortRoundTrip)
{
    const SortSpec sort{SortKey::TransferRate, true};
    const SortSpec parsed = ParseSort(SerializeSort(sort));
    EXPECT_EQ(parsed.key, SortKey::TransferRate);
    EXPECT_TRUE(parsed.descending);
    EXPECT_EQ(ParseSort("nonsense").key, SortKey::OrderOfAddition);
}

TEST(ListModel, ColumnLayoutRoundTripAndRepair)
{
    auto columns = DefaultColumns();
    ASSERT_EQ(columns.size(), kColumnCount);
    EXPECT_EQ(columns[0].id, Column::FileName);
    EXPECT_EQ(columns[1].id, Column::Queue);
    std::swap(columns[1], columns[3]);
    columns[0].visible = false;
    columns[1].width = 333;
    const auto parsed = ParseColumns(SerializeColumns(columns));
    ASSERT_EQ(parsed.size(), kColumnCount);
    EXPECT_EQ(parsed[1].id, Column::Status);
    EXPECT_EQ(parsed[1].width, 333);
    EXPECT_FALSE(parsed[0].visible);

    // Repeated, unknown and malformed entries are dropped; missing columns come back hidden.
    const auto repaired = ParseColumns("name:100:1,name:50:1,bogus:10:1,size:x:1,status:90:1");
    ASSERT_EQ(repaired.size(), kColumnCount);
    EXPECT_EQ(repaired[0].id, Column::FileName);
    EXPECT_EQ(repaired[1].id, Column::Status);
    EXPECT_FALSE(repaired[2].visible);
    EXPECT_EQ(ParseColumns("").size(), kColumnCount);
    EXPECT_TRUE(ParseColumns("name:100:0")[1].visible) << "a layout with nothing visible falls back to the default";
}

TEST(ListModel, FindCyclesThroughMatches)
{
    const Snapshot snapshot = Sample();
    const auto index = BuildIndex(snapshot, {}, {});
    FindOptions options;
    options.text = L"FILE";
    auto first = FindNext(snapshot, index, std::nullopt, options);
    ASSERT_EQ(first, 2u);
    auto second = FindNext(snapshot, index, first, options);
    ASSERT_EQ(second, 3u);
    EXPECT_EQ(FindNext(snapshot, index, second, options), 2u) << "wraps around (F3)";

    options.matchCase = true;
    EXPECT_FALSE(FindNext(snapshot, index, std::nullopt, options));
    options.matchCase = false;
    options.wholeString = true;
    options.text = L"file9.bin";
    EXPECT_EQ(FindNext(snapshot, index, std::nullopt, options), 3u);
}

}  // namespace
