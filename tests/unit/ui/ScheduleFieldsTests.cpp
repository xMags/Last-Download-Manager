#include "ui/ScheduleFields.h"

#include <gtest/gtest.h>

namespace {

using ldm::ui::CalendarDate;
using ldm::ui::DateFromDays;
using ldm::ui::DaysFromDate;

TEST(ScheduleFields, CalendarDaysCountFrom1970)
{
    EXPECT_EQ(DaysFromDate({1970, 1, 1}), 0);
    EXPECT_EQ(DaysFromDate({2027, 1, 15}), 20'833);
    EXPECT_EQ(DaysFromDate({1969, 12, 31}), -1);
    EXPECT_EQ(DateFromDays(20'833), (CalendarDate{2027, 1, 15}));
    EXPECT_EQ(DateFromDays(DaysFromDate({2028, 2, 29})), (CalendarDate{2028, 2, 29})) << "a leap day";
}

}  // namespace
