#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace ldm::engine {

// Byte-rate limiter shared by any number of connections (checklist P-04, Q-09). A connection may
// take a whole chunk while the balance is positive, which can push the balance below zero; it then
// waits until refills bring it back above zero. Over time the rate converges to the limit without
// splitting chunks. Not thread-safe: owned by the network thread.
class TokenBucket {
public:
    using Clock = std::chrono::steady_clock;

    // `bytesPerSecond` absent means unlimited.
    void SetRate(std::optional<std::int64_t> bytesPerSecond, Clock::time_point now) noexcept;
    [[nodiscard]] bool Limited() const noexcept { return rate_.has_value(); }

    // True if a chunk may be taken now. Does not consume.
    [[nodiscard]] bool CanTake(Clock::time_point now) noexcept;
    void Take(std::int64_t bytes) noexcept;

    // Time until CanTake() becomes true; zero when it already is.
    [[nodiscard]] Clock::duration TimeUntilAvailable(Clock::time_point now) noexcept;

private:
    void Refill(Clock::time_point now) noexcept;

    std::optional<std::int64_t> rate_;
    double balance_ = 0.0;
    Clock::time_point lastRefill_{};
};

}  // namespace ldm::engine
