#include "core/Quota.h"

#include <gtest/gtest.h>

namespace {

using ldm::core::QuotaPeriod;
using std::chrono::hours;
using std::chrono::minutes;

const QuotaPeriod::TimePoint kStart = QuotaPeriod::TimePoint{} + hours(5);
constexpr minutes kHour = hours(1);

TEST(QuotaPeriod, CountsWithinThePeriodAndStartsOverAfterIt)
{
    QuotaPeriod quota;
    EXPECT_EQ(quota.Used(kStart, kHour), 0);
    EXPECT_FALSE(quota.End(kStart, kHour));

    quota.Record(kStart, 100, kHour);
    quota.Record(kStart + minutes(30), 50, kHour);
    EXPECT_EQ(quota.Used(kStart + minutes(59), kHour), 150);
    EXPECT_EQ(quota.End(kStart + minutes(10), kHour), kStart + kHour);

    // The period ended: nothing is used until the next byte starts a new one.
    EXPECT_EQ(quota.Used(kStart + kHour, kHour), 0);
    quota.Record(kStart + minutes(70), 10, kHour);
    EXPECT_EQ(quota.Used(kStart + minutes(70), kHour), 10);
    EXPECT_EQ(quota.End(kStart + minutes(70), kHour), kStart + minutes(130));
}

TEST(QuotaPeriod, IgnoresEmptyRecordsAndFollowsALengthChange)
{
    QuotaPeriod quota;
    quota.Record(kStart, 0, kHour);
    quota.Record(kStart, -5, kHour);
    EXPECT_FALSE(quota.End(kStart, kHour));

    quota.Record(kStart, 100, kHour);
    EXPECT_EQ(quota.Used(kStart + minutes(90), hours(2)), 100) << "a longer period keeps the count";
    EXPECT_EQ(quota.Used(kStart + minutes(90), kHour), 0);
}

}  // namespace
