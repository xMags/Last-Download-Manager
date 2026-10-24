#include "core/Scheduler.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

namespace {

using ldm::core::kMsPerDay;
using ldm::core::kMsPerMinute;
using ldm::core::NextScheduledStart;
using ldm::core::NextScheduledStop;
using ldm::model::Schedule;
using ldm::model::ScheduleMode;

constexpr std::int64_t kHour = 60 * kMsPerMinute;
// 2027-01-15, a Friday.
constexpr std::int64_t kFriday = 20'833;

std::int64_t At(std::int64_t day, int hours, int minutes = 0)
{
    return day * kMsPerDay + hours * kHour + minutes * kMsPerMinute;
}

class FixedZone final : public ldm::core::TimeZone {
public:
    explicit FixedZone(std::int64_t offsetMs) : offset_(offsetMs) {}
    std::int64_t LocalToUnixMs(std::int64_t localMs) const override { return localMs - offset_; }
    std::int64_t UnixToLocalMs(std::int64_t unixMs) const override { return unixMs + offset_; }

private:
    std::int64_t offset_;
};

// Central European time: UTC+1, and UTC+2 from `spring` to `fall` (instants in UTC).
class EuropeZone final : public ldm::core::TimeZone {
public:
    EuropeZone(std::int64_t spring, std::int64_t fall) : spring_(spring), fall_(fall) {}
    std::int64_t UnixToLocalMs(std::int64_t unixMs) const override { return unixMs + (Summer(unixMs) ? 2 : 1) * kHour; }
    std::int64_t LocalToUnixMs(std::int64_t localMs) const override
    {
        const std::int64_t summer = localMs - 2 * kHour;
        const std::int64_t winter = localMs - kHour;
        if (Summer(summer)) {
            return summer;  // also the earlier of the two in the repeated hour
        }
        if (!Summer(winter)) {
            return winter;
        }
        return spring_;  // skipped when the clocks went forward
    }

private:
    bool Summer(std::int64_t unixMs) const { return unixMs >= spring_ && unixMs < fall_; }
    std::int64_t spring_;
    std::int64_t fall_;
};

const FixedZone kUtc{0};

Schedule Daily(int startMinutes, std::uint8_t weekdays = ldm::model::kEveryWeekday)
{
    Schedule schedule;
    schedule.enabled = true;
    schedule.mode = ScheduleMode::Daily;
    schedule.startMinutes = startMinutes;
    schedule.weekdays = weekdays;
    return schedule;
}

TEST(Scheduler, WeekdaysCountFromMonday)
{
    EXPECT_EQ(ldm::core::WeekdayOfLocalDay(0), 3);  // 1970-01-01, a Thursday
    EXPECT_EQ(ldm::core::WeekdayOfLocalDay(-1), 2);
    EXPECT_EQ(ldm::core::WeekdayOfLocalDay(kFriday), 4);
    EXPECT_EQ(ldm::core::WeekdayOfLocalDay(kFriday + 3), 0);
}

TEST(Scheduler, ADisabledScheduleNeverStartsOrStops)
{
    Schedule schedule = Daily(9 * 60);
    schedule.stopEnabled = true;
    schedule.enabled = false;
    EXPECT_FALSE(NextScheduledStart(schedule, At(kFriday, 0), kUtc));
    EXPECT_FALSE(NextScheduledStop(schedule, At(kFriday, 0), kUtc));
}

TEST(Scheduler, OnceStartsOnItsDayOnly)
{
    Schedule schedule;
    schedule.enabled = true;
    schedule.mode = ScheduleMode::Once;
    schedule.startMinutes = 10 * 60;
    schedule.startDay = kFriday;
    EXPECT_EQ(NextScheduledStart(schedule, At(kFriday, 8), kUtc), At(kFriday, 10));
    EXPECT_FALSE(NextScheduledStart(schedule, At(kFriday, 10), kUtc)) << "strictly after";
    schedule.startDay.reset();
    EXPECT_FALSE(NextScheduledStart(schedule, At(kFriday, 8), kUtc));
}

TEST(Scheduler, DailyStartsOnTheChosenWeekdays)
{
    const Schedule mondays = Daily(9 * 60, 0b0000001);
    EXPECT_EQ(NextScheduledStart(mondays, At(kFriday, 8), kUtc), At(kFriday + 3, 9));
    const Schedule everyDay = Daily(23 * 60 + 30);
    EXPECT_EQ(NextScheduledStart(everyDay, At(kFriday, 23, 45), kUtc), At(kFriday + 1, 23, 30));
    EXPECT_FALSE(NextScheduledStart(Daily(9 * 60, 0), At(kFriday, 8), kUtc)) << "no day chosen";
}

TEST(Scheduler, RepeatsUntilMidnightWithoutAStopTime)
{
    Schedule schedule = Daily(10 * 60);
    schedule.repeatMinutes = 90;
    EXPECT_EQ(NextScheduledStart(schedule, At(kFriday, 9), kUtc), At(kFriday, 10));
    EXPECT_EQ(NextScheduledStart(schedule, At(kFriday, 10), kUtc), At(kFriday, 11, 30));
    EXPECT_EQ(NextScheduledStart(schedule, At(kFriday, 22, 10), kUtc), At(kFriday, 23, 30));
    EXPECT_EQ(NextScheduledStart(schedule, At(kFriday, 23, 30), kUtc), At(kFriday + 1, 10));
}

TEST(Scheduler, RepeatsUntilTheStopTimeOnTheNextDay)
{
    Schedule schedule = Daily(22 * 60);
    schedule.repeatMinutes = 60;
    schedule.stopEnabled = true;
    schedule.stopMinutes = 2 * 60;
    EXPECT_EQ(NextScheduledStart(schedule, At(kFriday, 23, 10), kUtc), At(kFriday + 1, 0));
    EXPECT_EQ(NextScheduledStart(schedule, At(kFriday + 1, 0, 30), kUtc), At(kFriday + 1, 1));
    EXPECT_EQ(NextScheduledStart(schedule, At(kFriday + 1, 1), kUtc), At(kFriday + 1, 22)) << "not at the stop time";
}

TEST(Scheduler, RepeatingEveryMinuteFindsTheNextMinute)
{
    Schedule schedule = Daily(0);
    schedule.repeatMinutes = 1;
    EXPECT_EQ(NextScheduledStart(schedule, At(kFriday, 12, 34) + 30'000, kUtc), At(kFriday, 12, 35));
}

TEST(Scheduler, StopsOnTheDaysTheScheduleStarts)
{
    Schedule schedule = Daily(8 * 60);
    schedule.stopEnabled = true;
    schedule.stopMinutes = 17 * 60;
    EXPECT_EQ(NextScheduledStop(schedule, At(kFriday, 9), kUtc), At(kFriday, 17));

    Schedule night = Daily(22 * 60, 0b0010000);  // Fridays
    night.stopEnabled = true;
    night.stopMinutes = 6 * 60;
    EXPECT_EQ(NextScheduledStop(night, At(kFriday, 21), kUtc), At(kFriday + 1, 6)) << "an earlier stop is the next day";
    EXPECT_EQ(NextScheduledStop(night, At(kFriday + 1, 7), kUtc), At(kFriday + 8, 6));

    night.stopEnabled = false;
    EXPECT_FALSE(NextScheduledStop(night, At(kFriday, 21), kUtc));
}

TEST(Scheduler, OnceStopsOnItsDayOrTheNext)
{
    Schedule schedule;
    schedule.enabled = true;
    schedule.mode = ScheduleMode::Once;
    schedule.startDay = kFriday;
    schedule.startMinutes = 23 * 60;
    schedule.stopEnabled = true;
    schedule.stopMinutes = 1 * 60;
    EXPECT_EQ(NextScheduledStop(schedule, At(kFriday, 22), kUtc), At(kFriday + 1, 1));
}

TEST(Scheduler, LocalTimesFollowTheTimeZone)
{
    const FixedZone india{5 * kHour + 30 * kMsPerMinute};
    EXPECT_EQ(NextScheduledStart(Daily(9 * 60), At(kFriday, 0), india), At(kFriday, 3, 30));
    // 01:00 UTC on Friday is already Friday 06:30 there: the next 06:00 is Saturday's.
    EXPECT_EQ(NextScheduledStart(Daily(6 * 60), At(kFriday, 1), india), At(kFriday + 1, 0, 30));
}

TEST(Scheduler, DaylightSavingTimeKeepsTheLocalTime)
{
    // Clocks go forward on Sunday kFriday + 2 at 01:00 UTC (02:00 local becomes 03:00) and back
    // on Sunday kFriday + 9 at 01:00 UTC (03:00 local becomes 02:00).
    const std::int64_t spring = At(kFriday + 2, 1);
    const std::int64_t fall = At(kFriday + 9, 1);
    const EuropeZone zone{spring, fall};

    const Schedule nine = Daily(9 * 60);
    EXPECT_EQ(NextScheduledStart(nine, At(kFriday + 1, 9), zone), At(kFriday + 2, 7)) << "09:00 in summer time";
    EXPECT_EQ(NextScheduledStart(nine, At(kFriday + 9, 0), zone), At(kFriday + 9, 8)) << "09:00 in winter time";

    const Schedule skipped = Daily(2 * 60 + 30);
    EXPECT_EQ(NextScheduledStart(skipped, At(kFriday + 2, 0), zone), spring) << "a time that does not exist starts at the change";

    const auto repeated = NextScheduledStart(skipped, At(kFriday + 9, 0), zone);
    EXPECT_EQ(repeated, At(kFriday + 9, 0, 30)) << "a time that exists twice starts the first time";
    EXPECT_EQ(NextScheduledStart(skipped, *repeated, zone), At(kFriday + 10, 1, 30)) << "and only once";
}

}  // namespace
