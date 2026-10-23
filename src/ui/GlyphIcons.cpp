#include "ui/GlyphIcons.h"

#include <algorithm>
#include <cmath>
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

// Icons are drawn this many times larger, then averaged down, for smooth edges on drawn shapes.
constexpr int kSupersample = 4;
constexpr COLORREF kDisabledGradientColor = RGB(0xC0, 0xC0, 0xC0);
// Stroke width of drawn shapes in the 32-unit design grid, close to the icon font's line weight.
constexpr double kStroke = 1.4;

// A rounded rectangle in the 32-unit design grid of a drawn shape.
struct DesignRect {
    double left;
    double top;
    double right;
    double bottom;
    double radius;
};

void FillDesignRect(HDC dc, double unit, const DesignRect& r)
{
    const auto px = [unit](double v) { return static_cast<int>(std::lround(v * unit)); };
    const int diameter = std::max(0, px(2 * r.radius));
    ::RoundRect(dc, px(r.left), px(r.top), px(r.right), px(r.bottom), diameter, diameter);
}

DesignRect Inset(const DesignRect& r, double by)
{
    return DesignRect{r.left + by, r.top + by, r.right - by, r.bottom - by, std::max(0.0, r.radius - by)};
}

// Outlines a set of parts: the union of the parts minus the same parts shrunk by the stroke.
void OutlineParts(HDC dc, double unit, std::span<const DesignRect> parts)
{
    SelectObjectScope pen(dc, ::GetStockObject(NULL_PEN));
    {
        SelectObjectScope white(dc, ::GetStockObject(WHITE_BRUSH));
        for (const DesignRect& part : parts) {
            FillDesignRect(dc, unit, part);
        }
    }
    SelectObjectScope black(dc, ::GetStockObject(BLACK_BRUSH));
    for (const DesignRect& part : parts) {
        FillDesignRect(dc, unit, Inset(part, kStroke));
    }
}

void FillParts(HDC dc, double unit, std::span<const DesignRect> parts)
{
    SelectObjectScope pen(dc, ::GetStockObject(NULL_PEN));
    SelectObjectScope white(dc, ::GetStockObject(WHITE_BRUSH));
    for (const DesignRect& part : parts) {
        FillDesignRect(dc, unit, part);
    }
}

// A circle at the drawn-shape stroke; the icon font's ring is heavier than the reference's.
void DrawRing(HDC dc, int size)
{
    const double unit = size / 32.0;
    const auto px = [unit](double v) { return static_cast<int>(std::lround(v * unit)); };
    SelectObjectScope pen(dc, ::GetStockObject(NULL_PEN));
    {
        SelectObjectScope white(dc, ::GetStockObject(WHITE_BRUSH));
        ::Ellipse(dc, px(2.5), px(2.5), px(29.5), px(29.5));
    }
    SelectObjectScope black(dc, ::GetStockObject(BLACK_BRUSH));
    ::Ellipse(dc, px(2.5 + kStroke), px(2.5 + kStroke), px(29.5 - kStroke), px(29.5 - kStroke));
}

// A hollow plus: the outline of two crossed bars.
void DrawOutlinePlus(HDC dc, int size)
{
    const DesignRect bars[] = {{12.0, 2.5, 20.0, 29.5, 1.5}, {2.5, 12.0, 29.5, 20.0, 1.5}};
    OutlineParts(dc, size / 32.0, bars);
}

// A bin with a lid and a handle; `lines` adds the two strokes inside the bin.
void DrawTrash(HDC dc, int size, bool lines)
{
    const double unit = size / 32.0;
    const DesignRect handle[] = {{12.0, 3.0, 20.0, 9.0, 1.5}};
    OutlineParts(dc, unit, handle);
    const DesignRect body[] = {{7.5, 8.0, 24.5, 29.5, 3.0}};
    OutlineParts(dc, unit, body);
    const DesignRect lid[] = {{4.5, 7.5, 27.5, 7.5 + kStroke, kStroke / 2}};
    FillParts(dc, unit, lid);
    if (lines) {
        const DesignRect strokes[] = {{12.6, 13.0, 12.6 + kStroke, 25.0, kStroke / 2}, {18.0, 13.0, 18.0 + kStroke, 25.0, kStroke / 2}};
        FillParts(dc, unit, strokes);
    }
}

// An original open palm: four fingers, a thumb on the right and the palm, outlined.
void DrawRaisedHand(HDC dc, int size)
{
    const double unit = size / 32.0;
    constexpr double stroke = kStroke;
    constexpr double half = stroke / 2;
    constexpr double fingerWidth = 4.6;
    constexpr double left = 5.5;
    constexpr double fingerTops[4] = {5.0, 1.5, 2.5, 6.0};
    constexpr double fingerBottom = 18.0;
    const DesignRect palm{left, 14.0, left + 4 * fingerWidth, 30.0, 6.5};
    const DesignRect thumb{22.5, 14.5, 28.5, 25.0, 3.0};

    SelectObjectScope pen(dc, ::GetStockObject(NULL_PEN));
    {
        SelectObjectScope white(dc, ::GetStockObject(WHITE_BRUSH));
        for (int i = 0; i < 4; ++i) {
            const double x = left + i * fingerWidth;
            FillDesignRect(dc, unit, DesignRect{x, fingerTops[i], x + fingerWidth, fingerBottom, fingerWidth / 2});
        }
        FillDesignRect(dc, unit, palm);
        FillDesignRect(dc, unit, thumb);
    }
    SelectObjectScope black(dc, ::GetStockObject(BLACK_BRUSH));
    for (int i = 0; i < 4; ++i) {
        const double x = left + i * fingerWidth;
        // Outer edges keep the full stroke; edges shared with the next finger get half of it, so the
        // line between two fingers is as thick as the outline.
        const double insetLeft = i == 0 ? stroke : half;
        const double insetRight = i == 3 ? stroke : half;
        FillDesignRect(dc, unit,
                       DesignRect{x + insetLeft, fingerTops[i] + stroke, x + fingerWidth - insetRight, fingerBottom + stroke,
                                  fingerWidth / 2 - stroke});
    }
    FillDesignRect(dc, unit, DesignRect{palm.left + stroke, palm.top + stroke, palm.right - stroke, palm.bottom - stroke,
                                        palm.radius - stroke});
    FillDesignRect(dc, unit, DesignRect{thumb.left + half, thumb.top + stroke, thumb.right - stroke, thumb.bottom - stroke,
                                        thumb.radius - stroke});
}

void DrawLayer(HDC dc, int size, const IconLayer& layer)
{
    LOGFONTW logFont{};
    logFont.lfHeight = -std::max(1, size * layer.scalePercent / 100);
    logFont.lfQuality = ANTIALIASED_QUALITY;
    logFont.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(logFont.lfFaceName, IconFontFace(), _TRUNCATE);
    UniqueFont font(::CreateFontIndirectW(&logFont));
    SelectObjectScope selectFont(dc, font.get());
    const int dx = size * layer.offsetXPercent / 100;
    const int dy = size * layer.offsetYPercent / 100;
    RECT rect{dx, dy, size + dx, size + dy};
    const wchar_t text[2] = {layer.codepoint, 0};
    ::DrawTextW(dc, text, 1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

std::uint32_t Lerp(std::uint32_t from, std::uint32_t to, double t)
{
    return static_cast<std::uint32_t>(std::lround(from + (static_cast<double>(to) - from) * t));
}

Canvas RenderGradientIcon(const GradientIcon& icon, int size, bool grayed)
{
    Canvas result = CreateCanvas(size);
    const int big = size * kSupersample;
    Canvas mask = CreateCanvas(big);
    if (!result.bitmap || result.pixels == nullptr || !mask.bitmap || mask.pixels == nullptr) {
        return result;
    }
    const HDC dc = ::CreateCompatibleDC(nullptr);
    {
        SelectObjectScope selectBitmap(dc, mask.bitmap.get());
        RECT all{0, 0, big, big};
        ::FillRect(dc, &all, static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)));
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, RGB(255, 255, 255));
        switch (icon.shape) {
        case IconShape::Glyphs:
            break;
        case IconShape::Ring:
            DrawRing(dc, big);
            break;
        case IconShape::RaisedHand:
            DrawRaisedHand(dc, big);
            break;
        case IconShape::OutlinePlus:
            DrawOutlinePlus(dc, big);
            break;
        case IconShape::Trash:
        case IconShape::EmptyTrash:
            DrawTrash(dc, big, icon.shape == IconShape::Trash);
            break;
        }
        for (const IconLayer& layer : icon.layers) {
            if (layer.codepoint != 0) {
                DrawLayer(dc, big, layer);
            }
        }
    }
    ::DeleteDC(dc);
    ::GdiFlush();

    const COLORREF start = grayed ? kDisabledGradientColor : icon.start;
    const COLORREF end = grayed ? kDisabledGradientColor : icon.end;
    const double span = std::max(1, 2 * (size - 1));
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            std::uint32_t coverage = 0;
            for (int sy = 0; sy < kSupersample; ++sy) {
                const std::uint32_t* row =
                    mask.pixels + static_cast<size_t>(y * kSupersample + sy) * static_cast<size_t>(big) + x * kSupersample;
                for (int sx = 0; sx < kSupersample; ++sx) {
                    coverage += row[sx] & 0xFF;  // white drawing: any channel is the coverage
                }
            }
            const std::uint32_t alpha = coverage / (kSupersample * kSupersample);
            const double t = (x + y) / span;
            const std::uint32_t red = Lerp(GetRValue(start), GetRValue(end), t);
            const std::uint32_t green = Lerp(GetGValue(start), GetGValue(end), t);
            const std::uint32_t blue = Lerp(GetBValue(start), GetBValue(end), t);
            result.pixels[static_cast<size_t>(y) * static_cast<size_t>(size) + x] =
                (alpha << 24) | ((red * alpha / 255) << 16) | ((green * alpha / 255) << 8) | (blue * alpha / 255);
        }
    }
    return result;
}

}  // namespace

UniqueImageList CreateGradientImageList(std::span<const GradientIcon> icons, int sizePx, bool grayed)
{
    UniqueImageList list(::ImageList_Create(sizePx, sizePx, ILC_COLOR32, static_cast<int>(icons.size()), 4));
    for (const GradientIcon& icon : icons) {
        Canvas canvas = RenderGradientIcon(icon, sizePx, grayed);
        ::ImageList_Add(list.get(), canvas.bitmap.get(), nullptr);
    }
    return list;
}

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
