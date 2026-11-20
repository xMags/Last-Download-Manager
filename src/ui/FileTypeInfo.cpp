#include "ui/FileTypeInfo.h"

#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>

namespace ldm::ui {

namespace {

// A name the shell resolves by its extension alone (SHGFI_USEFILEATTRIBUTES).
std::wstring Probe(std::wstring_view extension)
{
    std::wstring probe = L"file";
    if (!extension.empty()) {
        probe.append(L".").append(extension);
    }
    return probe;
}

}  // namespace

UniqueIcon FileTypeIcon(std::wstring_view extension, int sizePx)
{
    SHFILEINFOW info{};
    const std::wstring probe = Probe(extension);
    if (::SHGetFileInfoW(probe.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                         SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX) == 0) {
        return {};
    }
    // The 256-pixel list for large tiles; below that the 48-pixel one, as some types' 256-pixel
    // image is only their 48-pixel icon in a corner.
    const int list = sizePx > 48 ? SHIL_JUMBO : SHIL_EXTRALARGE;
    IImageList* images = nullptr;
    if (FAILED(::SHGetImageList(list, IID_IImageList, reinterpret_cast<void**>(&images))) || images == nullptr) {
        return {};
    }
    HICON icon = nullptr;
    const HRESULT got = images->GetIcon(info.iIcon, ILD_TRANSPARENT, &icon);
    images->Release();
    return SUCCEEDED(got) ? UniqueIcon(icon) : UniqueIcon();
}

std::wstring FileTypeName(std::wstring_view extension)
{
    SHFILEINFOW info{};
    const std::wstring probe = Probe(extension);
    if (::SHGetFileInfoW(probe.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                         SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME) == 0) {
        return {};
    }
    return info.szTypeName;
}

}  // namespace ldm::ui
