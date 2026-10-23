#include "core/Quota.h"

namespace ldm::core {

bool QuotaPeriod::Running(TimePoint now, std::chrono::minutes length) const
{
    return start_.has_value() && now >= *start_ && now < *start_ + length;
}

void QuotaPeriod::Record(TimePoint now, std::int64_t bytes, std::chrono::minutes length)
{
    if (bytes <= 0) {
        return;
    }
    if (!Running(now, length)) {
        start_ = now;
        used_ = 0;
    }
    used_ += bytes;
}

std::int64_t QuotaPeriod::Used(TimePoint now, std::chrono::minutes length) const
{
    return Running(now, length) ? used_ : 0;
}

std::optional<QuotaPeriod::TimePoint> QuotaPeriod::End(TimePoint now, std::chrono::minutes length) const
{
    if (!Running(now, length)) {
        return std::nullopt;
    }
    return *start_ + length;
}

}  // namespace ldm::core
