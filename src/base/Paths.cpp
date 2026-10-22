#include "base/Paths.h"

#include <windows.h>

#include <limits>
#include <string_view>

namespace ldm::base {

std::wstring ToExtendedLengthPath(const std::filesystem::path& path)
{
    std::wstring text = path.lexically_normal().make_preferred().wstring();
    constexpr std::wstring_view kPrefix = LR"(\\?\)";
    constexpr std::wstring_view kUncPrefix = LR"(\\?\UNC\)";
    if (text.starts_with(kPrefix) || !path.is_absolute()) {
        return text;
    }
    if (text.starts_with(LR"(\\)")) {
        return std::wstring(kUncPrefix) + text.substr(2);
    }
    return std::wstring(kPrefix) + text;
}

std::wstring PathComparisonKey(const std::filesystem::path& path)
{
    std::wstring key = path.lexically_normal().make_preferred().wstring();
    while (key.size() > 3 && key.back() == L'\\') {
        key.pop_back();
    }
    if (!key.empty() && key.size() <= static_cast<size_t>(std::numeric_limits<DWORD>::max())) {
        ::CharUpperBuffW(key.data(), static_cast<DWORD>(key.size()));
    }
    return key;
}

}  // namespace ldm::base
