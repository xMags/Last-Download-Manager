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

// Drawn shapes are filled this many times larger, then averaged down, for smooth curves. Their
// edges and stroke widths are first snapped to whole pixels of the final size, so straight lines
// stay sharp instead of smearing across two pixels.
constexpr int kSupersample = 4;
// Matches the toolbar's disabled label text.
constexpr COLORREF kDisabledGradientColor = RGB(0xA0, 0xA0, 0xA0);
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

// A rounded rectangle in whole pixels of the final icon size.
struct PixelRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    int radius = 0;
};

// Lays shapes out on the final pixel grid and fills them on the supersampled canvas.
class ShapePainter {
public:
    ShapePainter(HDC dc, int size) : dc_(dc), unit_(size / 32.0), stroke_(std::max(1, static_cast<int>(std::lround(kStroke * size / 32.0)))) {}

    [[nodiscard]] int Stroke() const noexcept { return stroke_; }

    [[nodiscard]] PixelRect Snap(const DesignRect& r) const
    {
        return PixelRect{Px(r.left), Px(r.top), Px(r.right), Px(r.bottom), Px(r.radius)};
    }

    // Shrinks each side by its own number of pixels.
    [[nodiscard]] static PixelRect Inset(const PixelRect& r, int left, int top, int right, int bottom)
    {
        const int by = std::max(std::max(left, right), std::max(top, bottom));
        return PixelRect{r.left + left, r.top + top, r.right - right, r.bottom - bottom, std::max(0, r.radius - by)};
    }
    [[nodiscard]] PixelRect Inset(const PixelRect& r) const { return Inset(r, stroke_, stroke_, stroke_, stroke_); }

    void Fill(const PixelRect& r, bool white) const
    {
        SelectObjectScope pen(dc_, ::GetStockObject(NULL_PEN));
        SelectObjectScope brush(dc_, ::GetStockObject(white ? WHITE_BRUSH : BLACK_BRUSH));
        const int diameter = 2 * r.radius * kSupersample;
        // RoundRect leaves out the right and bottom edges; +1 covers the whole last pixel row.
        ::RoundRect(dc_, r.left * kSupersample, r.top * kSupersample, r.right * kSupersample + 1, r.bottom * kSupersample + 1,
                    diameter, diameter);
    }

    void FillEllipse(const PixelRect& r, bool white) const
    {
        SelectObjectScope pen(dc_, ::GetStockObject(NULL_PEN));
        SelectObjectScope brush(dc_, ::GetStockObject(white ? WHITE_BRUSH : BLACK_BRUSH));
        ::Ellipse(dc_, r.left * kSupersample, r.top * kSupersample, r.right * kSupersample + 1, r.bottom * kSupersample + 1);
    }

    // Outlines parts: the union of the parts minus the same parts shrunk by the stroke.
    void Outline(std::span<const PixelRect> parts) const
    {
        for (const PixelRect& part : parts) {
            Fill(part, true);
        }
        for (const PixelRect& part : parts) {
            Fill(Inset(part), false);
        }
    }

private:
    [[nodiscard]] int Px(double design) const { return static_cast<int>(std::lround(design * unit_)); }

    HDC dc_;
    double unit_;
    int stroke_;
};

// A circle at the drawn-shape stroke; the icon font's ring is too heavy next to the other icons.
void DrawRing(const ShapePainter& paint)
{
    const PixelRect outer = paint.Snap(DesignRect{2.5, 2.5, 29.5, 29.5, 0});
    paint.FillEllipse(outer, true);
    paint.FillEllipse(paint.Inset(outer), false);
}

// A hollow plus: the outline of two crossed bars.
void DrawOutlinePlus(const ShapePainter& paint)
{
    const PixelRect bars[] = {paint.Snap({12.0, 2.5, 20.0, 29.5, 1.5}), paint.Snap({2.5, 12.0, 29.5, 20.0, 1.5})};
    paint.Outline(bars);
}

// A bin with a lid and a handle; `lines` adds the two strokes inside the bin.
void DrawTrash(const ShapePainter& paint, bool lines)
{
    const PixelRect handle[] = {paint.Snap({12.0, 3.0, 20.0, 9.0, 1.5})};
    paint.Outline(handle);
    const PixelRect body[] = {paint.Snap({7.5, 8.0, 24.5, 29.5, 3.0})};
    paint.Outline(body);
    const int s = paint.Stroke();
    PixelRect lid = paint.Snap({4.5, 7.5, 27.5, 7.5, 0});
    lid.bottom = lid.top + s;
    paint.Fill(lid, true);
    if (lines) {
        for (const double x : {12.6, 18.0}) {
            PixelRect line = paint.Snap({x, 13.0, x, 25.0, 0});
            line.right = line.left + s;
            line.radius = s / 2;
            paint.Fill(line, true);
        }
    }
}

// An original open palm: four fingers, a thumb on the right and the palm, outlined.
void DrawRaisedHand(const ShapePainter& paint)
{
    constexpr double fingerWidth = 4.6;
    constexpr double left = 5.5;
    constexpr double fingerTops[4] = {5.0, 1.5, 2.5, 6.0};
    constexpr double fingerBottom = 18.0;
    const int s = paint.Stroke();
    PixelRect fingers[4];
    for (int i = 0; i < 4; ++i) {
        const double x = left + i * fingerWidth;
        fingers[i] = paint.Snap({x, fingerTops[i], x + fingerWidth, fingerBottom, fingerWidth / 2});
    }
    // The first finger has a stroke on both sides; widen it by one stroke so every finger shows
    // the same space inside.
    fingers[0].left -= s;
    PixelRect palm = paint.Snap({left, 14.0, left + 4 * fingerWidth, 30.0, 6.5});
    palm.left -= s;
    const PixelRect thumb = paint.Snap({22.5, 14.5, 28.5, 25.0, 3.0});
    for (const PixelRect& finger : fingers) {
        paint.Fill(finger, true);
    }
    paint.Fill(palm, true);
    paint.Fill(thumb, true);
    // Each finger keeps a stroke on its right only (and the first on its left too), so the line
    // between two fingers is one stroke wide, like the outline. The inside runs down into the palm.
    for (int i = 0; i < 4; ++i) {
        PixelRect inside = ShapePainter::Inset(fingers[i], i == 0 ? s : 0, s, s, 0);
        inside.bottom = palm.top + s + 1;
        paint.Fill(inside, false);
    }
    paint.Fill(paint.Inset(palm), false);
    paint.Fill(ShapePainter::Inset(thumb, 0, s, s, s), false);
}

void DrawLayer(HDC dc, int size, const IconLayer& layer)
{
    LOGFONTW logFont{};
    logFont.lfHeight = -std::max(1, size * layer.scalePercent / 100);
    logFont.lfQuality = ANTIALIASED_QUALITY;  // grayscale coverage; ClearType would tint the alpha
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

// Fills `canvas` black, runs `draw` on it with white as the ink, and flushes GDI.
template <typename Draw>
void DrawMask(const Canvas& canvas, int size, Draw draw)
{
    const HDC dc = ::CreateCompatibleDC(nullptr);
    {
        SelectObjectScope selectBitmap(dc, canvas.bitmap.get());
        RECT all{0, 0, size, size};
        ::FillRect(dc, &all, static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)));
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, RGB(255, 255, 255));
        draw(dc);
    }
    ::DeleteDC(dc);
    ::GdiFlush();
}

Canvas RenderGradientIcon(const GradientIcon& icon, int size, bool grayed)
{
    Canvas result = CreateCanvas(size);
    if (!result.bitmap || result.pixels == nullptr) {
        return result;
    }
    // Glyphs at their real size keep the font's hinting; shapes are drawn supersampled.
    Canvas glyphs = CreateCanvas(size);
    if (!glyphs.bitmap || glyphs.pixels == nullptr) {
        return result;
    }
    DrawMask(glyphs, size, [&icon, size](HDC dc) {
        for (const IconLayer& layer : icon.layers) {
            if (layer.codepoint != 0) {
                DrawLayer(dc, size, layer);
            }
        }
    });
    const int big = size * kSupersample;
    Canvas shapes;
    if (icon.shape != IconShape::Glyphs) {
        shapes = CreateCanvas(big);
        if (!shapes.bitmap || shapes.pixels == nullptr) {
            return result;
        }
        DrawMask(shapes, big, [&icon, size](HDC dc) {
            const ShapePainter paint(dc, size);
            switch (icon.shape) {
            case IconShape::Glyphs:
                break;
            case IconShape::Ring:
                DrawRing(paint);
                break;
            case IconShape::RaisedHand:
                DrawRaisedHand(paint);
                break;
            case IconShape::OutlinePlus:
                DrawOutlinePlus(paint);
                break;
            case IconShape::Trash:
            case IconShape::EmptyTrash:
                DrawTrash(paint, icon.shape == IconShape::Trash);
                break;
            }
        });
    }

    const COLORREF start = grayed ? kDisabledGradientColor : icon.start;
    const COLORREF end = grayed ? kDisabledGradientColor : icon.end;
    const double span = std::max(1, 2 * (size - 1));
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const size_t index = static_cast<size_t>(y) * static_cast<size_t>(size) + x;
            std::uint32_t alpha = glyphs.pixels[index] & 0xFF;  // white drawing: any channel is the coverage
            if (shapes.pixels != nullptr) {
                std::uint32_t coverage = 0;
                for (int sy = 0; sy < kSupersample; ++sy) {
                    const std::uint32_t* row =
                        shapes.pixels + static_cast<size_t>(y * kSupersample + sy) * static_cast<size_t>(big) + x * kSupersample;
                    for (int sx = 0; sx < kSupersample; ++sx) {
                        coverage += row[sx] & 0xFF;
                    }
                }
                alpha = std::max(alpha, coverage / (kSupersample * kSupersample));
            }
            const double t = (x + y) / span;
            const std::uint32_t red = Lerp(GetRValue(start), GetRValue(end), t);
            const std::uint32_t green = Lerp(GetGValue(start), GetGValue(end), t);
            const std::uint32_t blue = Lerp(GetBValue(start), GetBValue(end), t);
            result.pixels[index] = (alpha << 24) | ((red * alpha / 255) << 16) | ((green * alpha / 255) << 8) | (blue * alpha / 255);
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

UniqueFont CreateIconFont(int sizePx)
{
    LOGFONTW logFont{};
    logFont.lfHeight = -sizePx;
    logFont.lfQuality = CLEARTYPE_QUALITY;
    logFont.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(logFont.lfFaceName, IconFontFace(), _TRUNCATE);
    return UniqueFont(::CreateFontIndirectW(&logFont));
}

}  // namespace ldm::ui
