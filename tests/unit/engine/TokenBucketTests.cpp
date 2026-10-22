#include "engine/TokenBucket.h"

#include <gtest/gtest.h>

namespace {

using ldm::engine::TokenBucket;
using std::chrono::milliseconds;

TEST(TokenBucket, UnlimitedAlwaysAllows)
{
    TokenBucket bucket;
    const auto now = TokenBucket::Clock::now();
    bucket.SetRate(std::nullopt, now);
    bucket.Take(1'000'000'000);
    EXPECT_TRUE(bucket.CanTake(now));
    EXPECT_FALSE(bucket.Limited());
    bucket.SetRate(0, now);  // zero means no limit, never "stop forever"
    EXPECT_FALSE(bucket.Limited());
}

TEST(TokenBucket, ConvergesToTheConfiguredRate)
{
    // Simulated clock: a greedy consumer taking 16 KiB chunks whenever allowed, for 10 seconds.
    TokenBucket bucket;
    auto now = TokenBucket::Clock::time_point{} + std::chrono::hours(1);
    constexpr std::int64_t kRate = 100 * 1024;
    bucket.SetRate(kRate, now);
    const auto end = now + std::chrono::seconds(10);
    std::int64_t taken = 0;
    while (now < end) {
        if (bucket.CanTake(now)) {
            bucket.Take(16 * 1024);
            taken += 16 * 1024;
        } else {
            now += std::max<TokenBucket::Clock::duration>(bucket.TimeUntilAvailable(now), milliseconds(1));
        }
    }
    const double rate = static_cast<double>(taken) / 10.0;
    EXPECT_NEAR(rate, static_cast<double>(kRate), kRate * 0.05);
}

TEST(TokenBucket, BurstIsBoundedAfterIdle)
{
    TokenBucket bucket;
    auto now = TokenBucket::Clock::time_point{} + std::chrono::hours(1);
    bucket.SetRate(1024 * 1024, now);
    now += std::chrono::minutes(5);  // a long idle period must not bank minutes of tokens
    std::int64_t taken = 0;
    while (bucket.CanTake(now)) {
        bucket.Take(64 * 1024);
        taken += 64 * 1024;
    }
    EXPECT_LE(taken, 1024 * 1024 / 4 + 64 * 1024);
}

}  // namespace
