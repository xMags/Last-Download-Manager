#pragma once

#include <cstdint>
#include <optional>

namespace ldm::model {

// A byte range of the file assigned to one connection. `end` is exclusive and absent while the
// total size is unknown. `done` counts bytes from `start` that are durably on disk.
struct Segment {
    std::int64_t start = 0;
    std::optional<std::int64_t> end;
    std::int64_t done = 0;

    [[nodiscard]] std::int64_t WriteOffset() const noexcept { return start + done; }
    [[nodiscard]] bool IsComplete() const noexcept { return end.has_value() && start + done >= *end; }
};

}  // namespace ldm::model
