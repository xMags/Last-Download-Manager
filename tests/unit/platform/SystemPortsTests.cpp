#include "platform/win/SystemPorts.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

constexpr std::int64_t kHourMs = 3'600'000;
constexpr std::int64_t kDayMs = 24 * kHourMs;

// Whatever zone this computer is set to: conversions go both ways and stay within the offsets
// time zones have. 18:00 UTC is never in an hour that clocks repeat when they go back.
TEST(WinTimeZone, ConvertsBothWaysInThisComputersZone)
{
    const ldm::platform::WinTimeZone zone;
    const std::int64_t january = 20'833 * kDayMs + 18 * kHourMs;  // 2027-01-15
    for (const std::int64_t unixMs : {january, january + 181 * kDayMs}) {
        const std::int64_t local = zone.UnixToLocalMs(unixMs);
        EXPECT_LE(local - unixMs, 14 * kHourMs);
        EXPECT_GE(local - unixMs, -12 * kHourMs);
        EXPECT_EQ(zone.LocalToUnixMs(local), unixMs);
    }
}

}  // namespace
