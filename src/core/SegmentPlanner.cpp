#include "core/SegmentPlanner.h"

namespace ldm::core {

std::optional<PlanStep> ChooseWork(std::span<const PlanSegment> segments, const PlannerConfig& config)
{
    std::optional<size_t> bestIdle;
    std::optional<size_t> bestBusy;
    for (size_t i = 0; i < segments.size(); ++i) {
        const PlanSegment& segment = segments[i];
        if (segment.Remaining() == 0) {
            continue;
        }
        std::optional<size_t>& best = segment.assigned ? bestBusy : bestIdle;
        if (!best || segment.Remaining() > segments[*best].Remaining()) {
            best = i;
        }
    }
    if (bestIdle) {
        return PlanStep{PlanStep::Kind::Resume, *bestIdle, 0};
    }
    if (bestBusy) {
        const PlanSegment& busy = segments[*bestBusy];
        const std::int64_t remaining = busy.Remaining();
        if (remaining >= config.minSplitBytes) {
            return PlanStep{PlanStep::Kind::Split, *bestBusy, busy.position + remaining / 2};
        }
    }
    return std::nullopt;
}

}  // namespace ldm::core
