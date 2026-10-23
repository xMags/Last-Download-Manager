#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

// Numbers written in manifests. Both parsers accept only plain digits: no signs, spaces, hex or
// exponents, and nothing that does not fit.
namespace ldm::media {

// One or more decimal digits that fit in 64 bits.
[[nodiscard]] std::optional<std::uint64_t> ParseUnsigned(std::string_view text);

// Digits with an optional fraction ("4", "4.5", ".5"), finite and not negative.
[[nodiscard]] std::optional<double> ParseNonNegativeDecimal(std::string_view text);

// a + b, or nothing when it would not fit in 64 bits.
[[nodiscard]] std::optional<std::uint64_t> CheckedAdd(std::uint64_t a, std::uint64_t b);

}  // namespace ldm::media
