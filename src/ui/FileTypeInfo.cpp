#include "ui/FileTypeInfo.h"

#include <windows.h>

#include <shellapi.h>

namespace ldm::ui {

std::wstring FileTypeName(std::wstring_view extension)
{
    // A name the shell resolves by its extension alone (SHGFI_USEFILEATTRIBUTES).
    std::wstring probe = L"file";
    if (!extension.empty()) {
        probe.append(L".").append(extension);
    }
    SHFILEINFOW info{};
    if (::SHGetFileInfoW(probe.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                         SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME) == 0) {
        return {};
    }
    return info.szTypeName;
}

}  // namespace ldm::ui
