#include "ui/ScheduleFields.h"

#include <chrono>

namespace ldm::ui {

std::int64_t DaysFromDate(const CalendarDate& date) noexcept
{
    const std::chrono::year_month_day ymd{std::chrono::year{date.year}, std::chrono::month{date.month},
                                          std::chrono::day{date.day}};
    return std::chrono::sys_days{ymd}.time_since_epoch().count();
}

CalendarDate DateFromDays(std::int64_t days) noexcept
{
    const std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{days}}};
    return CalendarDate{static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day())};
}

}  // namespace ldm::ui
