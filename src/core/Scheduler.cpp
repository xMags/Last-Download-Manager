#include "core/Scheduler.h"

#include <algorithm>

namespace ldm::core {

namespace {

// Days looked at around the starting point: a weekly schedule always has a day in this range.
constexpr std::int64_t kDaysBefore = 1;
constexpr std::int64_t kDaysAfter = 9;

std::int64_t FloorDiv(std::int64_t value, std::int64_t divisor) noexcept
{
    const std::int64_t quotient = value / divisor;
    return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

std::int64_t LocalMs(std::int64_t day, int minutes) noexcept
{
    return day * kMsPerDay + std::int64_t{minutes} * kMsPerMinute;
}

bool RunsOn(const model::Schedule& schedule, std::int64_t day) noexcept
{
    return (schedule.weekdays & (1u << WeekdayOfLocalDay(day))) != 0;
}

// A stop time that is not later than the start time is on the next day.
std::int64_t StopDayOffset(const model::Schedule& schedule) noexcept
{
    return schedule.stopMinutes <= schedule.startMinutes ? 1 : 0;
}

std::int64_t FirstDayToLookAt(std::int64_t afterUnixMs, const TimeZone& zone)
{
    return FloorDiv(zone.UnixToLocalMs(afterUnixMs), kMsPerDay) - kDaysBefore;
}

}  // namespace

int WeekdayOfLocalDay(std::int64_t localDay) noexcept
{
    // 1970-01-01 was a Thursday, weekday 3.
    const std::int64_t weekday = (localDay + 3) % 7;
    return static_cast<int>(weekday < 0 ? weekday + 7 : weekday);
}

std::optional<std::int64_t> NextScheduledStart(const model::Schedule& schedule, std::int64_t afterUnixMs,
                                               const TimeZone& zone)
{
    if (!schedule.enabled) {
        return std::nullopt;
    }
    if (schedule.mode == model::ScheduleMode::Once) {
        if (!schedule.startDay) {
            return std::nullopt;
        }
        const std::int64_t at = zone.LocalToUnixMs(LocalMs(*schedule.startDay, schedule.startMinutes));
        return at > afterUnixMs ? std::optional<std::int64_t>(at) : std::nullopt;
    }
    if ((schedule.weekdays & model::kEveryWeekday) == 0) {
        return std::nullopt;
    }
    const std::int64_t afterLocal = zone.UnixToLocalMs(afterUnixMs);
    const std::int64_t step = std::int64_t{schedule.repeatMinutes} * kMsPerMinute;
    const std::int64_t firstDay = FirstDayToLookAt(afterUnixMs, zone);
    for (std::int64_t day = firstDay; day <= firstDay + kDaysBefore + kDaysAfter; ++day) {
        if (!RunsOn(schedule, day)) {
            continue;
        }
        const std::int64_t first = LocalMs(day, schedule.startMinutes);
        const std::int64_t end = schedule.stopEnabled ? LocalMs(day + StopDayOffset(schedule), schedule.stopMinutes)
                                                      : LocalMs(day + 1, 0);
        std::int64_t local = first;
        if (step > 0 && afterLocal > first) {
            // Skip the repeats long before `after`; one step back covers a clock change between.
            local = first + std::max<std::int64_t>(0, (afterLocal - first) / step - 1) * step;
        }
        for (; local < end; local += step) {  // `end` is always later than the first start
            const std::int64_t at = zone.LocalToUnixMs(local);
            if (at > afterUnixMs) {
                return at;
            }
            if (step == 0) {
                break;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::int64_t> NextScheduledStop(const model::Schedule& schedule, std::int64_t afterUnixMs,
                                              const TimeZone& zone)
{
    if (!schedule.enabled || !schedule.stopEnabled) {
        return std::nullopt;
    }
    if (schedule.mode == model::ScheduleMode::Once) {
        if (!schedule.startDay) {
            return std::nullopt;
        }
        const std::int64_t at =
            zone.LocalToUnixMs(LocalMs(*schedule.startDay + StopDayOffset(schedule), schedule.stopMinutes));
        return at > afterUnixMs ? std::optional<std::int64_t>(at) : std::nullopt;
    }
    if ((schedule.weekdays & model::kEveryWeekday) == 0) {
        return std::nullopt;
    }
    const std::int64_t firstDay = FirstDayToLookAt(afterUnixMs, zone);
    for (std::int64_t day = firstDay; day <= firstDay + kDaysBefore + kDaysAfter; ++day) {
        if (!RunsOn(schedule, day)) {
            continue;
        }
        const std::int64_t at = zone.LocalToUnixMs(LocalMs(day + StopDayOffset(schedule), schedule.stopMinutes));
        if (at > afterUnixMs) {
            return at;
        }
    }
    return std::nullopt;
}

}  // namespace ldm::core
