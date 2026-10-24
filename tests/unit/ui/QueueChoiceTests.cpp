#include "ui/QueueChoice.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace {

using ldm::core::DownloadView;
using ldm::model::DownloadId;
using ldm::model::DownloadStatus;
using ldm::model::Queue;
using ldm::model::QueueId;
using ldm::model::QueueKind;

DownloadView View(std::int64_t id, DownloadStatus status, std::optional<QueueId> queue = std::nullopt)
{
    DownloadView view;
    view.download.id = DownloadId{id};
    view.download.status = status;
    view.download.queueId = queue;
    return view;
}

Queue MakeQueue(std::int64_t id, QueueKind kind = QueueKind::Custom)
{
    Queue queue;
    queue.id = QueueId{id};
    queue.kind = kind;
    return queue;
}

std::vector<std::int64_t> Values(const std::vector<DownloadId>& ids)
{
    std::vector<std::int64_t> values;
    for (const DownloadId id : ids) {
        values.push_back(id.value);
    }
    return values;
}

TEST(QueueChoice, CompletedDownloadsJoinAnOrdinaryQueueOnlyWhenTheUserSaysSo)
{
    const DownloadView paused = View(1, DownloadStatus::Paused);
    const DownloadView done = View(2, DownloadStatus::Completed);
    const DownloadView member = View(3, DownloadStatus::Queued, QueueId{5});
    const DownloadView deleting = View(4, DownloadStatus::Deleting);
    const std::vector<const DownloadView*> selection{&paused, &done, &member, &deleting};

    const Queue target = MakeQueue(5);
    EXPECT_EQ(ldm::ui::CompletedJoiners(selection, target), 1);
    EXPECT_EQ(Values(ldm::ui::QueueJoiners(selection, target, true)), (std::vector<std::int64_t>{1, 2}))
        << "already a member and being deleted are left out";
    EXPECT_EQ(Values(ldm::ui::QueueJoiners(selection, target, false)), (std::vector<std::int64_t>{1}));
}

TEST(QueueChoice, TheSynchronizationQueueTakesCompletedDownloadsWithoutAsking)
{
    const DownloadView done = View(2, DownloadStatus::Completed);
    const std::vector<const DownloadView*> selection{&done};
    const Queue sync = MakeQueue(2, QueueKind::Sync);
    EXPECT_EQ(ldm::ui::CompletedJoiners(selection, sync), 0);
    EXPECT_EQ(Values(ldm::ui::QueueJoiners(selection, sync, false)), (std::vector<std::int64_t>{2}));
}

TEST(QueueChoice, FindsQueuesInTheSnapshot)
{
    ldm::core::Snapshot snapshot;
    snapshot.queues.push_back({MakeQueue(1, QueueKind::Main)});
    snapshot.queues.push_back({MakeQueue(4)});
    EXPECT_EQ(ldm::ui::MainQueueId(snapshot), QueueId{1});
    ASSERT_NE(ldm::ui::FindQueueView(snapshot, QueueId{4}), nullptr);
    EXPECT_EQ(ldm::ui::FindQueueView(snapshot, QueueId{9}), nullptr);
    EXPECT_FALSE(ldm::ui::MainQueueId(ldm::core::Snapshot{}));
}

}  // namespace
