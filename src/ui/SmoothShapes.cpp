#include "ui/SmoothShapes.h"

#include "ui/Handles.h"

#include <algorithm>
#include <vector>
#include <objidl.h>

// GDI+ uses unqualified min and max, which NOMINMAX (set for the whole build) removes.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

namespace ldm::ui {

namespace {

Gdiplus::Color ToColor(COLORREF color)
{
    return Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color));
}

// The outline of a rounded rectangle covering whole pixels from `rect.left` to `rect.right`.
void AddRoundedRect(Gdiplus::GraphicsPath& path, const RECT& rect, int radius)
{
    const auto left = static_cast<Gdiplus::REAL>(rect.left);
    const auto top = static_cast<Gdiplus::REAL>(rect.top);
    const auto width = static_cast<Gdiplus::REAL>(rect.right - rect.left);
    const auto height = static_cast<Gdiplus::REAL>(rect.bottom - rect.top);
    const Gdiplus::REAL diameter = std::min({static_cast<Gdiplus::REAL>(2 * radius), width, height});
    if (diameter <= 0) {
        path.AddRectangle(Gdiplus::RectF(left, top, width, height));
        return;
    }
    path.AddArc(left, top, diameter, diameter, 180, 90);
    path.AddArc(left + width - diameter, top, diameter, diameter, 270, 90);
    path.AddArc(left + width - diameter, top + height - diameter, diameter, diameter, 0, 90);
    path.AddArc(left, top + height - diameter, diameter, diameter, 90, 90);
    path.CloseFigure();
}

// False when GDI+ is not running (it failed to start), so the caller draws with GDI.
bool Ready(const Gdiplus::Graphics& graphics)
{
    return graphics.GetLastStatus() == Gdiplus::Ok;
}

void Prepare(Gdiplus::Graphics& graphics)
{
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    // Pixel edges sit on whole coordinates, so a filled shape covers exactly its pixels.
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
}

}  // namespace

GdiPlusSession::GdiPlusSession()
{
    Gdiplus::GdiplusStartupInput input;
    started_ = Gdiplus::GdiplusStartup(&token_, &input, nullptr) == Gdiplus::Ok;
}

GdiPlusSession::~GdiPlusSession()
{
    if (started_) {
        Gdiplus::GdiplusShutdown(token_);
    }
}

void FillRoundedRect(HDC dc, const RECT& rect, int radius, COLORREF color)
{
    Gdiplus::Graphics graphics(dc);
    if (Ready(graphics)) {
        Prepare(graphics);
        Gdiplus::GraphicsPath path;
        AddRoundedRect(path, rect, radius);
        Gdiplus::SolidBrush brush(ToColor(color));
        if (graphics.FillPath(&brush, &path) == Gdiplus::Ok) {
            return;
        }
    }
    UniqueBrush brush(::CreateSolidBrush(color));
    const SelectObjectScope selectBrush(dc, brush.get());
    const SelectObjectScope selectPen(dc, ::GetStockObject(NULL_PEN));
    // RoundRect leaves out the right and bottom edges; +1 covers them.
    ::RoundRect(dc, rect.left, rect.top, rect.right + 1, rect.bottom + 1, 2 * radius, 2 * radius);
}

void FrameRoundedRect(HDC dc, const RECT& rect, int radius, int width, COLORREF color)
{
    Gdiplus::Graphics graphics(dc);
    if (Ready(graphics)) {
        Prepare(graphics);
        // The pen is centred on the path, so the path runs half a pen inside `rect`.
        const int inset = width / 2;
        const RECT inner{rect.left + inset, rect.top + inset, rect.right - (width - inset), rect.bottom - (width - inset)};
        Gdiplus::GraphicsPath path;
        AddRoundedRect(path, inner, std::max(0, radius - inset));
        Gdiplus::Pen pen(ToColor(color), static_cast<Gdiplus::REAL>(width));
        if (graphics.DrawPath(&pen, &path) == Gdiplus::Ok) {
            return;
        }
    }
    UniquePen pen(::CreatePen(PS_INSIDEFRAME, width, color));
    const SelectObjectScope selectPen(dc, pen.get());
    const SelectObjectScope selectBrush(dc, ::GetStockObject(NULL_BRUSH));
    ::RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, 2 * radius, 2 * radius);
}

void FillCircle(HDC dc, const RECT& bounds, COLORREF color)
{
    Gdiplus::Graphics graphics(dc);
    if (Ready(graphics)) {
        Prepare(graphics);
        Gdiplus::SolidBrush brush(ToColor(color));
        if (graphics.FillEllipse(&brush, static_cast<Gdiplus::REAL>(bounds.left), static_cast<Gdiplus::REAL>(bounds.top),
                                 static_cast<Gdiplus::REAL>(bounds.right - bounds.left),
                                 static_cast<Gdiplus::REAL>(bounds.bottom - bounds.top)) == Gdiplus::Ok) {
            return;
        }
    }
    UniqueBrush brush(::CreateSolidBrush(color));
    const SelectObjectScope selectBrush(dc, brush.get());
    const SelectObjectScope selectPen(dc, ::GetStockObject(NULL_PEN));
    ::Ellipse(dc, bounds.left, bounds.top, bounds.right + 1, bounds.bottom + 1);
}

void DrawCheckMark(HDC dc, POINT start, POINT corner, POINT end, int width, COLORREF color)
{
    const POINT points[] = {start, corner, end};
    DrawLines(dc, points, width, color);
}

void DrawLines(HDC dc, std::span<const POINT> points, int width, COLORREF color)
{
    if (points.size() < 2) {
        return;
    }
    Gdiplus::Graphics graphics(dc);
    if (Ready(graphics)) {
        Prepare(graphics);
        Gdiplus::Pen pen(ToColor(color), static_cast<Gdiplus::REAL>(width));
        pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        std::vector<Gdiplus::Point> path;
        for (const POINT& point : points) {
            path.emplace_back(point.x, point.y);
        }
        if (graphics.DrawLines(&pen, path.data(), static_cast<INT>(path.size())) == Gdiplus::Ok) {
            return;
        }
    }
    LOGBRUSH brush{BS_SOLID, color, 0};
    UniquePen pen(::ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, static_cast<DWORD>(width),
                                 &brush, 0, nullptr));
    const SelectObjectScope selectPen(dc, pen.get());
    ::Polyline(dc, points.data(), static_cast<int>(points.size()));
}

void FillPolygon(HDC dc, std::span<const POINT> points, COLORREF fill, COLORREF line, int width)
{
    if (points.size() < 3) {
        return;
    }
    Gdiplus::Graphics graphics(dc);
    if (Ready(graphics)) {
        Prepare(graphics);
        std::vector<Gdiplus::Point> path;
        for (const POINT& point : points) {
            path.emplace_back(point.x, point.y);
        }
        Gdiplus::SolidBrush brush(ToColor(fill));
        Gdiplus::Pen pen(ToColor(line), static_cast<Gdiplus::REAL>(width));
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        if (graphics.FillPolygon(&brush, path.data(), static_cast<INT>(path.size())) == Gdiplus::Ok &&
            graphics.DrawPolygon(&pen, path.data(), static_cast<INT>(path.size())) == Gdiplus::Ok) {
            return;
        }
    }
    UniqueBrush brush(::CreateSolidBrush(fill));
    UniquePen pen(::CreatePen(PS_SOLID, width, line));
    const SelectObjectScope selectBrush(dc, brush.get());
    const SelectObjectScope selectPen(dc, pen.get());
    ::Polygon(dc, points.data(), static_cast<int>(points.size()));
}

}  // namespace ldm::ui
