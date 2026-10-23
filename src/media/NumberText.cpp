#include "media/NumberText.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>

namespace ldm::media {

namespace {

bool IsDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

}  // namespace

std::optional<std::uint64_t> ParseUnsigned(std::string_view text)
{
    if (text.empty() || !std::all_of(text.begin(), text.end(), IsDigit)) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> ParseNonNegativeDecimal(std::string_view text)
{
    const size_t dot = text.find('.');
    const std::string_view whole = text.substr(0, dot);
    const std::string_view fraction = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    if (whole.empty() && fraction.empty()) {
        return std::nullopt;
    }
    if (!std::all_of(whole.begin(), whole.end(), IsDigit) || !std::all_of(fraction.begin(), fraction.end(), IsDigit)) {
        return std::nullopt;
    }
    double value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::fixed);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint64_t> CheckedAdd(std::uint64_t a, std::uint64_t b)
{
    if (a > std::numeric_limits<std::uint64_t>::max() - b) {
        return std::nullopt;
    }
    return a + b;
}

}  // namespace ldm::media
