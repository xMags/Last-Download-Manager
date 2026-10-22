#include "engine/TokenBucket.h"

#include <algorithm>

namespace ldm::engine {

namespace {

// Burst capacity as a fraction of one second of the rate. Small enough that a limit is visible
// within a second, large enough that a 16 KiB chunk is never starved at low limits.
constexpr double kBurstSeconds = 0.25;
constexpr double kMinimumBurstBytes = 16.0 * 1024.0;

}  // namespace

void TokenBucket::SetRate(std::optional<std::int64_t> bytesPerSecond, Clock::time_point now) noexcept
{
    if (bytesPerSecond && *bytesPerSecond <= 0) {
        bytesPerSecond.reset();
    }
    rate_ = bytesPerSecond;
    balance_ = 0.0;
    lastRefill_ = now;
}

void TokenBucket::Refill(Clock::time_point now) noexcept
{
    if (!rate_) {
        return;
    }
    if (now > lastRefill_) {
        const double seconds = std::chrono::duration<double>(now - lastRefill_).count();
        const double capacity = std::max(static_cast<double>(*rate_) * kBurstSeconds, kMinimumBurstBytes);
        balance_ = std::min(balance_ + seconds * static_cast<double>(*rate_), capacity);
    }
    lastRefill_ = now;
}

bool TokenBucket::CanTake(Clock::time_point now) noexcept
{
    if (!rate_) {
        return true;
    }
    Refill(now);
    return balance_ > 0.0;
}

void TokenBucket::Take(std::int64_t bytes) noexcept
{
    if (rate_) {
        balance_ -= static_cast<double>(bytes);
    }
}

TokenBucket::Clock::duration TokenBucket::TimeUntilAvailable(Clock::time_point now) noexcept
{
    if (CanTake(now)) {
        return Clock::duration::zero();
    }
    const double seconds = (-balance_ + 1.0) / static_cast<double>(*rate_);
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}

}  // namespace ldm::engine
