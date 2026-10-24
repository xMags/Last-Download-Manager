#include "core/QueueRunner.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using ldm::core::PlanQueue;
using ldm::core::QueueMember;
using ldm::model::DownloadId;
using ldm::model::DownloadStatus;
using ldm::model::Queue;
using ldm::model::QueueKind;

Queue MakeQueue(int concurrent, int retries = 3, QueueKind kind = QueueKind::Custom)
{
    Queue queue;
    queue.name = "Test";
    queue.kind = kind;
    queue.concurrent = concurrent;
    queue.retriesPerFile = retries;
    return queue;
}

QueueMember Member(std::int64_t id, std::int64_t position, DownloadStatus status = DownloadStatus::Queued)
{
    QueueMember member;
    member.id = DownloadId{id};
    member.position = position;
    member.status = status;
    return member;
}

std::vector<std::int64_t> Ids(const std::vector<DownloadId>& ids)
{
    std::vector<std::int64_t> values;
    for (const DownloadId id : ids) {
        values.push_back(id.value);
    }
    return values;
}

TEST(QueueRunner, StartsMembersInPositionOrderUpToTheConcurrency)
{
    const auto plan = PlanQueue(MakeQueue(2), {Member(1, 30), Member(2, 10), Member(3, 20)}, true);
    EXPECT_EQ(Ids(plan.start), (std::vector<std::int64_t>{2, 3}));
    EXPECT_FALSE(plan.finished);
}

TEST(QueueRunner, EqualPositionsGoInIdOrder)
{
    const auto plan = PlanQueue(MakeQueue(1), {Member(9, 1), Member(4, 1)}, true);
    EXPECT_EQ(Ids(plan.start), (std::vector<std::int64_t>{4}));
}

TEST(QueueRunner, BusyMembersTakeSlots)
{
    QueueMember running = Member(1, 1, DownloadStatus::Downloading);
    running.busy = true;
    const auto full = PlanQueue(MakeQueue(1), {running, Member(2, 2)}, true);
    EXPECT_TRUE(full.start.empty());
    EXPECT_FALSE(full.finished);

    const auto one = PlanQueue(MakeQueue(2), {running, Member(2, 2), Member(3, 3)}, true);
    EXPECT_EQ(Ids(one.start), (std::vector<std::int64_t>{2}));
}

TEST(QueueRunner, MoreBusyMembersThanSlotsStartNothing)
{
    QueueMember a = Member(1, 1, DownloadStatus::Downloading);
    QueueMember b = Member(2, 2, DownloadStatus::Downloading);
    a.busy = b.busy = true;  // started by hand beside the queue
    const auto plan = PlanQueue(MakeQueue(1), {a, b, Member(3, 3)}, true);
    EXPECT_TRUE(plan.start.empty());
}

TEST(QueueRunner, PausedMembersResume)
{
    const auto plan = PlanQueue(MakeQueue(1), {Member(1, 1, DownloadStatus::Paused)}, true);
    EXPECT_EQ(Ids(plan.start), (std::vector<std::int64_t>{1}));
}

TEST(QueueRunner, FailedMembersAreTriedAgainWhileRetriesAreLeft)
{
    QueueMember failed = Member(1, 1, DownloadStatus::Failed);
    failed.failures = 3;
    EXPECT_EQ(Ids(PlanQueue(MakeQueue(1, 3), {failed}, true).start), (std::vector<std::int64_t>{1}));

    failed.failures = 4;
    const auto done = PlanQueue(MakeQueue(1, 3), {failed}, true);
    EXPECT_TRUE(done.start.empty());
    EXPECT_TRUE(done.finished) << "a member out of retries does not keep the queue going";

    failed.failures = 1;
    EXPECT_TRUE(PlanQueue(MakeQueue(1, 0), {failed}, true).start.empty()) << "no retries: one attempt";
}

TEST(QueueRunner, ACompletedMemberStartsAgainUntilItIsHeld)
{
    QueueMember completed = Member(1, 1, DownloadStatus::Completed);
    EXPECT_EQ(Ids(PlanQueue(MakeQueue(1), {completed}, true).start), (std::vector<std::int64_t>{1}));
    EXPECT_EQ(Ids(PlanQueue(MakeQueue(1, 3, QueueKind::Sync), {completed}, true).start), (std::vector<std::int64_t>{1}))
        << "the synchronization queue checks it";

    completed.held = true;  // checked in this run already
    const auto sync = PlanQueue(MakeQueue(1, 3, QueueKind::Sync), {completed}, true);
    EXPECT_TRUE(sync.start.empty());
    EXPECT_TRUE(sync.finished);
}

TEST(QueueRunner, HeldMembersAreSkipped)
{
    QueueMember held = Member(1, 1);
    held.held = true;
    const auto plan = PlanQueue(MakeQueue(1), {held, Member(2, 2)}, true);
    EXPECT_EQ(Ids(plan.start), (std::vector<std::int64_t>{2}));

    const auto alone = PlanQueue(MakeQueue(1), {held}, true);
    EXPECT_TRUE(alone.finished);
}

TEST(QueueRunner, StatesInBetweenNeitherStartNorKeepTheQueueGoing)
{
    const auto plan = PlanQueue(MakeQueue(2), {Member(1, 1, DownloadStatus::Deleting)}, true);
    EXPECT_TRUE(plan.start.empty());
    EXPECT_TRUE(plan.finished);
}

TEST(QueueRunner, AnEmptyQueueIsDone)
{
    EXPECT_TRUE(PlanQueue(MakeQueue(3), {}, true).finished);
}

TEST(QueueRunner, ABusyMemberKeepsTheQueueGoing)
{
    QueueMember finalizing = Member(1, 1, DownloadStatus::Finalizing);
    finalizing.busy = true;
    EXPECT_FALSE(PlanQueue(MakeQueue(1), {finalizing}, true).finished);
}

TEST(QueueRunner, WhileTheQuotaStopsNewRunsNothingStartsAndTheQueueWaits)
{
    const auto plan = PlanQueue(MakeQueue(2), {Member(1, 1), Member(2, 2)}, false);
    EXPECT_TRUE(plan.start.empty());
    EXPECT_FALSE(plan.finished);
}

}  // namespace
