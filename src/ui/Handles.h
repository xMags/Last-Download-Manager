#pragma once

#include <windows.h>
#include <commctrl.h>

#include <memory>
#include <type_traits>

// RAII owners for the Win32 handles the UI creates (design section 6). Each deleter matches the
// function that frees that kind of handle.
namespace ldm::ui {

struct GdiObjectDeleter {
    void operator()(HGDIOBJ object) const noexcept { ::DeleteObject(object); }
};
struct IconDeleter {
    void operator()(HICON icon) const noexcept { ::DestroyIcon(icon); }
};
struct ImageListDeleter {
    void operator()(HIMAGELIST list) const noexcept { ::ImageList_Destroy(list); }
};
struct MenuDeleter {
    void operator()(HMENU menu) const noexcept { ::DestroyMenu(menu); }
};
struct WindowDeleter {
    void operator()(HWND window) const noexcept { ::DestroyWindow(window); }
};

using UniqueFont = std::unique_ptr<std::remove_pointer_t<HFONT>, GdiObjectDeleter>;
using UniqueBrush = std::unique_ptr<std::remove_pointer_t<HBRUSH>, GdiObjectDeleter>;
using UniqueBitmap = std::unique_ptr<std::remove_pointer_t<HBITMAP>, GdiObjectDeleter>;
using UniquePen = std::unique_ptr<std::remove_pointer_t<HPEN>, GdiObjectDeleter>;
using UniqueIcon = std::unique_ptr<std::remove_pointer_t<HICON>, IconDeleter>;
using UniqueImageList = std::unique_ptr<std::remove_pointer_t<HIMAGELIST>, ImageListDeleter>;
using UniqueMenu = std::unique_ptr<std::remove_pointer_t<HMENU>, MenuDeleter>;
using UniqueWindow = std::unique_ptr<std::remove_pointer_t<HWND>, WindowDeleter>;

// Selects a GDI object into a device context for the lifetime of the scope.
class SelectObjectScope {
public:
    SelectObjectScope(HDC dc, HGDIOBJ object) noexcept : dc_(dc), previous_(::SelectObject(dc, object)) {}
    ~SelectObjectScope() { ::SelectObject(dc_, previous_); }
    SelectObjectScope(const SelectObjectScope&) = delete;
    SelectObjectScope& operator=(const SelectObjectScope&) = delete;

private:
    HDC dc_;
    HGDIOBJ previous_;
};

}  // namespace ldm::ui
