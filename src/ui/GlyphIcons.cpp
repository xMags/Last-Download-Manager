#include "ui/GlyphIcons.h"

#include <algorithm>
#include <cstdint>
#include <cwchar>

namespace ldm::ui {

namespace {

bool FontInstalled(const wchar_t* face)
{
    LOGFONTW query{};
    query.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(query.lfFaceName, face, _TRUNCATE);
    bool found = false;
    const HDC screen = ::GetDC(nullptr);
    ::EnumFontFamiliesExW(
        screen, &query,
        [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found) -> int {
            *reinterpret_cast<bool*>(found) = true;  // LPARAM carries the flag's address
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);
    ::ReleaseDC(nullptr, screen);
    return found;
}

const wchar_t* IconFontFace()
{
    static const wchar_t* const face = FontInstalled(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    return face;
}

// A top-down 32-bit DIB section the caller can write pixels into.
struct Canvas {
    UniqueBitmap bitmap;
    std::uint32_t* pixels = nullptr;
};

Canvas CreateCanvas(int size)
{
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    Canvas canvas;
    canvas.bitmap.reset(::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    canvas.pixels = static_cast<std::uint32_t*>(bits);
    return canvas;
}

// Draws the glyph white on black, then turns brightness into alpha and fills with the color, so
// the result has smooth, premultiplied edges at any size.
Canvas RenderGlyph(const Glyph& glyph, int size, bool grayed)
{
    Canvas canvas = CreateCanvas(size);
    if (!canvas.bitmap || canvas.pixels == nullptr) {
        return canvas;
    }
    const HDC dc = ::CreateCompatibleDC(nullptr);
    {
        SelectObjectScope selectBitmap(dc, canvas.bitmap.get());
        LOGFONTW logFont{};
        logFont.lfHeight = -size;
        logFont.lfQuality = ANTIALIASED_QUALITY;  // grayscale coverage; ClearType would tint the alpha
        logFont.lfCharSet = DEFAULT_CHARSET;
        wcsncpy_s(logFont.lfFaceName, IconFontFace(), _TRUNCATE);
        UniqueFont font(::CreateFontIndirectW(&logFont));
        SelectObjectScope selectFont(dc, font.get());
        RECT rect{0, 0, size, size};
        ::FillRect(dc, &rect, static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)));
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, RGB(255, 255, 255));
        const wchar_t text[2] = {glyph.codepoint, 0};
        ::DrawTextW(dc, text, 1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    ::DeleteDC(dc);
    ::GdiFlush();

    const COLORREF color = grayed ? RGB(0x9A, 0x9A, 0x9A) : glyph.color;
    const std::uint32_t red = GetRValue(color);
    const std::uint32_t green = GetGValue(color);
    const std::uint32_t blue = GetBValue(color);
    for (int i = 0; i < size * size; ++i) {
        const std::uint32_t alpha = canvas.pixels[i] & 0xFF;  // white text: any channel is the coverage
        canvas.pixels[i] = (alpha << 24) | ((red * alpha / 255) << 16) | ((green * alpha / 255) << 8) | (blue * alpha / 255);
    }
    return canvas;
}

}  // namespace

UniqueImageList CreateGlyphImageList(std::span<const Glyph> glyphs, int sizePx, bool grayed)
{
    UniqueImageList list(::ImageList_Create(sizePx, sizePx, ILC_COLOR32, static_cast<int>(glyphs.size()), 4));
    for (const Glyph& glyph : glyphs) {
        Canvas canvas = RenderGlyph(glyph, sizePx, grayed);
        ::ImageList_Add(list.get(), canvas.bitmap.get(), nullptr);
    }
    return list;
}

UniqueIcon CreateGlyphIcon(const Glyph& glyph, int sizePx)
{
    Canvas canvas = RenderGlyph(glyph, sizePx, false);
    UniqueBitmap mask(::CreateBitmap(sizePx, sizePx, 1, 1, nullptr));
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = canvas.bitmap.get();
    info.hbmMask = mask.get();
    return UniqueIcon(::CreateIconIndirect(&info));
}

}  // namespace ldm::ui
