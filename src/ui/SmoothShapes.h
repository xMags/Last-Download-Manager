#pragma once

#include <windows.h>

#include <span>

namespace ldm::ui {

// GDI+ for the process, for the smooth (anti-aliased) shapes below. Create one before the first
// window and destroy it after the last one is gone. If GDI+ fails to start, the shapes are drawn
// with plain GDI instead: the same shapes, with stepped edges.
class GdiPlusSession {
public:
    GdiPlusSession();
    ~GdiPlusSession();
    GdiPlusSession(const GdiPlusSession&) = delete;
    GdiPlusSession& operator=(const GdiPlusSession&) = delete;

private:
    ULONG_PTR token_ = 0;
    bool started_ = false;
};

// Shapes with smooth edges, drawn onto `dc` in its own coordinates. `radius` in pixels.
void FillRoundedRect(HDC dc, const RECT& rect, int radius, COLORREF color);
void FrameRoundedRect(HDC dc, const RECT& rect, int radius, int width, COLORREF color);
void FillCircle(HDC dc, const RECT& bounds, COLORREF color);
// A check mark through three points, `width` pixels wide, with round ends.
void DrawCheckMark(HDC dc, POINT start, POINT corner, POINT end, int width, COLORREF color);
// A polygon filled with `fill` and outlined `width` pixels wide in `line`, with round joins.
void FillPolygon(HDC dc, std::span<const POINT> points, COLORREF fill, COLORREF line, int width);
// Connected lines `width` pixels wide with round joins.
void DrawLines(HDC dc, std::span<const POINT> points, int width, COLORREF color);

}  // namespace ldm::ui
