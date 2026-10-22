#include "base/Paths.h"

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

}  // namespace ldm::base
