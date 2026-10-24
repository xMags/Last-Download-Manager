#pragma once

#include "core/Ports.h"
#include "model/Schedule.h"

#include <cstdint>
#include <optional>

// When a schedule starts and stops its queue (M4 design section 4.1). Pure: times are Unix
// milliseconds, converted from the schedule's local times through the time zone.
//   Once:  starts at `startMinutes` on `startDay`.
//   Daily: starts at `startMinutes` on each day in `weekdays`, and with `repeatMinutes` again
//          every that many minutes until the stop time, when there is one, or else midnight.
//   Stop:  at `stopMinutes` on each day the schedule starts on, or the next day when the stop
//          time is not later than the start time.
// A schedule that is not enabled has no starts and no stops.
namespace ldm::core {

inline constexpr std::int64_t kMsPerMinute = 60'000;
inline constexpr std::int64_t kMsPerDay = 24 * 60 * kMsPerMinute;

// A start or stop is carried out when LDM notices it this long after its time at most: after the
// computer woke up, or the clock was moved. Older ones are missed, never caught up (decision 4).
inline constexpr std::int64_t kScheduleGraceMs = 15 * kMsPerMinute;

// The first start strictly after `afterUnixMs`.
[[nodiscard]] std::optional<std::int64_t> NextScheduledStart(const model::Schedule& schedule, std::int64_t afterUnixMs,
                                                             const TimeZone& zone);
// The first stop strictly after `afterUnixMs`.
[[nodiscard]] std::optional<std::int64_t> NextScheduledStop(const model::Schedule& schedule, std::int64_t afterUnixMs,
                                                            const TimeZone& zone);

// Monday is 0 and Sunday 6, matching the bits of Schedule::weekdays.
[[nodiscard]] int WeekdayOfLocalDay(std::int64_t localDay) noexcept;

}  // namespace ldm::core
