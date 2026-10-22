#include "ui/SystemActions.h"

namespace ldm::ui {

std::wstring SubstituteFileArgument(std::wstring_view arguments, const std::wstring& file)
{
    const std::wstring quoted = L"\"" + file + L"\"";
    constexpr std::wstring_view kPlaceholder = L"[File]";
    std::wstring result;
    bool replaced = false;
    size_t pos = 0;
    while (pos < arguments.size()) {
        const size_t found = arguments.find(kPlaceholder, pos);
        if (found == std::wstring_view::npos) {
            result.append(arguments.substr(pos));
            break;
        }
        result.append(arguments.substr(pos, found - pos));
        // A placeholder the user already quoted gets the bare path.
        const bool alreadyQuoted = found > 0 && arguments[found - 1] == L'"' && found + kPlaceholder.size() < arguments.size() &&
                                   arguments[found + kPlaceholder.size()] == L'"';
        result.append(alreadyQuoted ? file : quoted);
        replaced = true;
        pos = found + kPlaceholder.size();
    }
    if (!replaced) {
        if (!result.empty()) {
            result.push_back(L' ');
        }
        result.append(quoted);
    }
    return result;
}

}  // namespace ldm::ui
