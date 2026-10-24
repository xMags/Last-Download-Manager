#pragma once

#include <compare>
#include <cstdint>
#include <functional>

namespace ldm::model {

// Database row identifiers. Distinct types so a category id can never be passed where a
// download id is expected. These ids never leave the process; see Download::publicId.
struct DownloadId {
    std::int64_t value = 0;
    auto operator<=>(const DownloadId&) const = default;
};

struct CategoryId {
    std::int64_t value = 0;
    auto operator<=>(const CategoryId&) const = default;
};

struct QueueId {
    std::int64_t value = 0;
    auto operator<=>(const QueueId&) const = default;
};

}  // namespace ldm::model

template <>
struct std::hash<ldm::model::DownloadId> {
    size_t operator()(const ldm::model::DownloadId& id) const noexcept
    {
        return std::hash<std::int64_t>{}(id.value);
    }
};

template <>
struct std::hash<ldm::model::QueueId> {
    size_t operator()(const ldm::model::QueueId& id) const noexcept
    {
        return std::hash<std::int64_t>{}(id.value);
    }
};
