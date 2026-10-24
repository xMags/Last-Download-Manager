#pragma once

#include <cstdint>

// Conversions between a schedule's stored values and the Scheduler window's date fields (M4
// design section 7). Pure, for tests.
namespace ldm::ui {

struct CalendarDate {
    int year = 1970;
    unsigned month = 1;  // 1 to 12
    unsigned day = 1;    // 1 to 31

    bool operator==(const CalendarDate&) const = default;
};

// Days since 1970-01-01, as Schedule::startDay stores a local calendar day.
[[nodiscard]] std::int64_t DaysFromDate(const CalendarDate& date) noexcept;
[[nodiscard]] CalendarDate DateFromDays(std::int64_t days) noexcept;

}  // namespace ldm::ui
