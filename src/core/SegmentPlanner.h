#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace ldm::core {

// Planner view of one segment of a download with a known total size.
struct PlanSegment {
    std::int64_t start = 0;
    std::int64_t end = 0;       // exclusive
    std::int64_t position = 0;  // next byte to receive (start + bytes received so far)
    bool assigned = false;      // a connection is currently serving this segment

    [[nodiscard]] std::int64_t Remaining() const noexcept { return end > position ? end - position : 0; }
};

struct PlannerConfig {
    // A segment is split only if at least this many bytes remain, so both halves stay useful.
    std::int64_t minSplitBytes = 1024 * 1024;
};

// Work for one free connection.
struct PlanStep {
    enum class Kind {
        Resume,  // serve the unassigned segment `segmentIndex` from its position
        Split,   // shorten segment `segmentIndex` to end at `splitAt`; the new connection takes [splitAt, old end)
    };
    Kind kind = Kind::Resume;
    size_t segmentIndex = 0;
    std::int64_t splitAt = 0;
};

// Chooses work for one newly available connection (checklist ENG-01, ENG-02):
// 1. an unassigned, incomplete segment, largest remaining first;
// 2. otherwise split the segment with the largest remaining bytes at the midpoint of what remains,
//    if that is at least config.minSplitBytes;
// 3. otherwise nothing: extra connections would not help.
[[nodiscard]] std::optional<PlanStep> ChooseWork(std::span<const PlanSegment> segments, const PlannerConfig& config);

}  // namespace ldm::core
