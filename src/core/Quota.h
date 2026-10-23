#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace ldm::core {

// The download quota's accounting (checklist O-05): "no more than N MB every H hours". A period
// starts with the first byte received after the previous period ended and lasts `length`; its
// count starts again at zero when it ends. Not thread-safe; the coordinator owns it.
class QuotaPeriod {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    void Record(TimePoint now, std::int64_t bytes, std::chrono::minutes length);

    // Bytes received in the period running at `now`; zero when none is running.
    [[nodiscard]] std::int64_t Used(TimePoint now, std::chrono::minutes length) const;
    // When the period running at `now` ends; nothing when none is running.
    [[nodiscard]] std::optional<TimePoint> End(TimePoint now, std::chrono::minutes length) const;

private:
    [[nodiscard]] bool Running(TimePoint now, std::chrono::minutes length) const;

    std::optional<TimePoint> start_;
    std::int64_t used_ = 0;
};

}  // namespace ldm::core
