#include "media/MediaModel.h"
#include "media/NumberText.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace {

using ldm::media::CheckedAdd;
using ldm::media::ParseNonNegativeDecimal;
using ldm::media::ParseUnsigned;

TEST(NumberText, ParsesUnsignedIntegers)
{
    EXPECT_EQ(ParseUnsigned("0"), 0u);
    EXPECT_EQ(ParseUnsigned("007"), 7u);
    EXPECT_EQ(ParseUnsigned("18446744073709551615"), std::numeric_limits<std::uint64_t>::max());
    for (const char* text : {"", "18446744073709551616", "-1", "+1", " 1", "1 ", "0x10", "1.0", "1e3"}) {
        EXPECT_FALSE(ParseUnsigned(text)) << text;
    }
}

TEST(NumberText, ParsesNonNegativeDecimals)
{
    EXPECT_EQ(ParseNonNegativeDecimal("4"), 4.0);
    EXPECT_EQ(ParseNonNegativeDecimal("4.5"), 4.5);
    EXPECT_EQ(ParseNonNegativeDecimal(".5"), 0.5);
    EXPECT_EQ(ParseNonNegativeDecimal("4."), 4.0);
    EXPECT_EQ(ParseNonNegativeDecimal("0.000"), 0.0);
    for (const char* text : {"", ".", "-1", "+1", "1e3", "inf", "nan", "1.2.3", " 1", "1,5"}) {
        EXPECT_FALSE(ParseNonNegativeDecimal(text)) << text;
    }
}

TEST(NumberText, AddsWithoutOverflow)
{
    EXPECT_EQ(CheckedAdd(1, 2), 3u);
    EXPECT_EQ(CheckedAdd(std::numeric_limits<std::uint64_t>::max() - 1, 1), std::numeric_limits<std::uint64_t>::max());
    EXPECT_FALSE(CheckedAdd(std::numeric_limits<std::uint64_t>::max(), 1));
}

TEST(MediaModel, TotalSecondsSumsTheSegments)
{
    ldm::media::TrackSegments track;
    track.segments.resize(3);
    track.segments[0].seconds = 4;
    track.segments[1].seconds = 4;
    track.segments[2].seconds = 1.5;
    EXPECT_DOUBLE_EQ(ldm::media::TotalSeconds(track), 9.5);
}

}  // namespace
