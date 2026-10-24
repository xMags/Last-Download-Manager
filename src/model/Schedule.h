#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace ldm::model {

// When a queue starts and stops by itself (checklist Q-02). Grabber projects use the same rules.
enum class ScheduleMode {
    Once,   // on one calendar day
    Daily,  // on the chosen weekdays
};

[[nodiscard]] std::string_view ToString(ScheduleMode mode) noexcept;
[[nodiscard]] std::optional<ScheduleMode> ParseScheduleMode(std::string_view text) noexcept;

inline constexpr int kMinutesPerDay = 24 * 60;
inline constexpr std::uint8_t kEveryWeekday = 0x7F;

struct Schedule {
    bool enabled = false;
    ScheduleMode mode = ScheduleMode::Daily;
    int startMinutes = 0;                 // local time of day, 0 to kMinutesPerDay - 1
    std::optional<std::int64_t> startDay;  // Once: the local calendar day, as days since 1970-01-01
    std::uint8_t weekdays = kEveryWeekday;  // Daily: bit 0 is Monday, bit 6 Sunday
    int repeatMinutes = 0;                // "start again every"; 0 is off
    bool stopEnabled = false;
    int stopMinutes = 0;  // local time of day; earlier than the start means the next day
    bool startOnAppStart = false;
    std::optional<std::int64_t> lastStartedAtMs;  // Unix ms; a Once schedule that fired turns itself off

    bool operator==(const Schedule&) const = default;
};

}  // namespace ldm::model
