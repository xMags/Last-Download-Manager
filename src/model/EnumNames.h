#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace ldm::model::detail {

// Lookups in a table of an enum's values and their stored names, for the model's ToString and
// Parse functions.
template <typename Enum, std::size_t N>
[[nodiscard]] constexpr std::string_view NameOf(const std::array<std::pair<Enum, std::string_view>, N>& table,
                                                Enum value) noexcept
{
    for (const auto& [candidate, name] : table) {
        if (candidate == value) {
            return name;
        }
    }
    return {};
}

template <typename Enum, std::size_t N>
[[nodiscard]] constexpr std::optional<Enum> ValueOf(const std::array<std::pair<Enum, std::string_view>, N>& table,
                                                    std::string_view text) noexcept
{
    for (const auto& [value, name] : table) {
        if (name == text) {
            return value;
        }
    }
    return std::nullopt;
}

}  // namespace ldm::model::detail
