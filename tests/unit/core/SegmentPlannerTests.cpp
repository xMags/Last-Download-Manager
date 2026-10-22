#include "core/SegmentPlanner.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using ldm::core::ChooseWork;
using ldm::core::PlannerConfig;
using ldm::core::PlanSegment;
using ldm::core::PlanStep;

constexpr std::int64_t kMiB = 1024 * 1024;

TEST(SegmentPlanner, SplitsTheOnlyRunningSegmentAtMidpointOfRemainder)
{
    // The first connection has received 1 MiB of a 17 MiB file.
    const std::vector<PlanSegment> segments{{0, 17 * kMiB, 1 * kMiB, true}};
    const auto step = ChooseWork(segments, PlannerConfig{});
    ASSERT_TRUE(step);
    EXPECT_EQ(step->kind, PlanStep::Kind::Split);
    EXPECT_EQ(step->segmentIndex, 0u);
    EXPECT_EQ(step->splitAt, 9 * kMiB);
}

TEST(SegmentPlanner, SplitsLargestRemainingNotLargestSegment)
{
    const std::vector<PlanSegment> segments{
        {0, 10 * kMiB, 9 * kMiB, true},       // 1 MiB left
        {10 * kMiB, 14 * kMiB, 10 * kMiB, true},  // 4 MiB left
    };
    const auto step = ChooseWork(segments, PlannerConfig{});
    ASSERT_TRUE(step);
    EXPECT_EQ(step->segmentIndex, 1u);
    EXPECT_EQ(step->splitAt, 12 * kMiB);
}

TEST(SegmentPlanner, PrefersUnassignedSegmentsOverSplitting)
{
    // After a restart every segment is idle; free connections pick them up before splitting.
    const std::vector<PlanSegment> segments{
        {0, 8 * kMiB, 8 * kMiB, false},           // complete
        {8 * kMiB, 10 * kMiB, 9 * kMiB, false},   // 1 MiB left
        {10 * kMiB, 20 * kMiB, 11 * kMiB, true},  // 9 MiB left, busy
        {20 * kMiB, 24 * kMiB, 20 * kMiB, false}, // 4 MiB left
    };
    const auto step = ChooseWork(segments, PlannerConfig{});
    ASSERT_TRUE(step);
    EXPECT_EQ(step->kind, PlanStep::Kind::Resume);
    EXPECT_EQ(step->segmentIndex, 3u);
}

TEST(SegmentPlanner, RefusesSplitBelowMinimum)
{
    const std::vector<PlanSegment> segments{{0, 3 * kMiB, 2 * kMiB + 1, true}};
    EXPECT_FALSE(ChooseWork(segments, PlannerConfig{}));
}

TEST(SegmentPlanner, NothingToDoWhenComplete)
{
    const std::vector<PlanSegment> segments{{0, 100, 100, false}, {100, 200, 200, false}};
    EXPECT_FALSE(ChooseWork(segments, PlannerConfig{}));
    EXPECT_FALSE(ChooseWork({}, PlannerConfig{}));
}

TEST(SegmentPlanner, RepeatedSplitsConvergeToEvenWork)
{
    // Simulate eight connections joining a 64 MiB download that has barely started.
    std::vector<PlanSegment> segments{{0, 64 * kMiB, 0, true}};
    for (int connection = 1; connection < 8; ++connection) {
        const auto step = ChooseWork(segments, PlannerConfig{});
        ASSERT_TRUE(step);
        ASSERT_EQ(step->kind, PlanStep::Kind::Split);
        PlanSegment& split = segments[step->segmentIndex];
        const std::int64_t oldEnd = split.end;
        split.end = step->splitAt;
        segments.push_back({step->splitAt, oldEnd, step->splitAt, true});
    }
    ASSERT_EQ(segments.size(), 8u);
    for (const PlanSegment& segment : segments) {
        EXPECT_EQ(segment.Remaining(), 8 * kMiB);
    }
}

}  // namespace
