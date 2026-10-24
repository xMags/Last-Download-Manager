#pragma once

#include <compare>
#include <cstdint>

namespace ldm::core {

// A piece of a stream's track that is now on disk in full: reported by the engine, applied by the
// coordinator, stored by the store.
struct MediaPieceDone {
    std::uint32_t track = 0;
    std::uint32_t index = 0;
    std::int64_t bytes = 0;  // the size of its file

    auto operator<=>(const MediaPieceDone&) const = default;
};

}  // namespace ldm::core
