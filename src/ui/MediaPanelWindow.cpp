#include "ui/MediaPanelWindow.h"

#include "resource.h"
#include "ui/Dpi.h"

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <string_view>

namespace ldm::ui {

namespace {

constexpr wchar_t kClassName[] = L"LdmMediaPanel";
constexpr UINT_PTR kTimerId = 1;
constexpr std::wstring_view kDownloadText = L"Download this video";
constexpr std::wstring_view kProtectedText = L"Protected video";

// Sizes at 96 DPI.
constexpr int kHeight96 = 28;
constexpr int kPadding96 = 8;
constexpr int kIcon96 = 16;
constexpr int kGap96 = 6;
constexpr int kArrow96 = 9;
constexpr int kSideButton96 = 22;
constexpr int kTextSlack96 = 4;

UINT DpiAt(POINT point)
{
    const HMONITOR monitor = ::MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = USER_DEFAULT_SCREEN_DPI;
    UINT dpiY = USER_DEFAULT_SCREEN_DPI;
    if (FAILED(::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        return USER_DEFAULT_SCREEN_DPI;
    }
    return dpiX;
}

int TextWidth(HFONT font, std::wstring_view text)
{
    const HDC dc = ::GetDC(nullptr);
    const HGDIOBJ previous = ::SelectObject(dc, font);
    SIZE size{};
    ::GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &size);
    ::SelectObject(dc, previous);
    ::ReleaseDC(nullptr, dc);
    return size.cx;
}

void DrawLine(HDC dc, int x1, int y1, int x2, int y2)
{
    ::MoveToEx(dc, x1, y1, nullptr);
    ::LineTo(dc, x2, y2);
}

}  // namespace

MediaPanelWindow::~MediaPanelWindow()
{
    Destroy();
}

bool MediaPanelWindow::Create(const Theme& theme, Callbacks callbacks)
{
    if (window_ != nullptr) {
        return true;
    }
    theme_ = &theme;
    callbacks_ = std::move(callbacks);
    static const ATOM atom = [] {
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.style = CS_DROPSHADOW;
        windowClass.lpfnWndProc = &MediaPanelWindow::WindowProc;
        windowClass.hInstance = ::GetModuleHandleW(nullptr);
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_HAND);
        windowClass.lpszClassName = kClassName;
        return ::RegisterClassExW(&windowClass);
    }();
    (void)atom;
    window_ = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kClassName,
                                L"Last Download Manager media panel", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                ::GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
        return false;
    }
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUNDSMALL;
    ::DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    UpdateFont(DpiOf(window_));
    return true;
}

void MediaPanelWindow::Destroy()
{
    if (window_ == nullptr) {
        return;
    }
    const HWND window = window_;
    window_ = nullptr;
    visible_ = false;
    ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    ::DestroyWindow(window);
}

void MediaPanelWindow::UpdateFont(UINT dpi)
{
    dpi_ = dpi;
    font_ = CreateMessageFont(dpi);
    const int iconSize = Scale(kIcon96, dpi);
    HICON icon = nullptr;
    ::LoadIconWithScaleDown(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_LDM), iconSize, iconSize, &icon);
    icon_.reset(icon);
}

SIZE MediaPanelWindow::SizeForDpi(UINT dpi) const
{
    const int height = Scale(kHeight96, dpi);
    if (mini_) {
        return SIZE{Scale(kPadding96 + kIcon96 + kGap96 + kArrow96 + kPadding96, dpi), height};
    }
    const std::wstring_view text = protected_ ? kProtectedText : kDownloadText;
    const UniqueFont measuring = dpi == dpi_ ? UniqueFont() : CreateMessageFont(dpi);
    const int textWidth = TextWidth(measuring ? measuring.get() : font_.get(), text);
    // A few pixels of slack: the parts are scaled one by one when drawn, and their rounding must
    // never cut the text short.
    const int width = Scale(kPadding96 + kIcon96 + kGap96, dpi) + textWidth +
                      Scale(kGap96 + kArrow96 + kPadding96 + 2 * kSideButton96 + kTextSlack96, dpi);
    return SIZE{width, height};
}

SIZE MediaPanelWindow::SizeAt(POINT at) const
{
    return SizeForDpi(DpiAt(at));
}

void MediaPanelWindow::ShowAt(POINT topLeft)
{
    if (window_ == nullptr) {
        return;
    }
    const UINT dpi = DpiAt(topLeft);
    if (dpi != dpi_) {
        UpdateFont(dpi);
    }
    const SIZE size = SizeForDpi(dpi);
    RECT current{};
    ::GetWindowRect(window_, &current);
    if (visible_ && current.left == topLeft.x && current.top == topLeft.y && current.right - current.left == size.cx &&
        current.bottom - current.top == size.cy) {
        return;  // nothing moved: no repaint, no flicker
    }
    ::SetWindowPos(window_, HWND_TOPMOST, topLeft.x, topLeft.y, size.cx, size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;
}

void MediaPanelWindow::Hide()
{
    if (window_ != nullptr && visible_) {
        ::ShowWindow(window_, SW_HIDE);
    }
    visible_ = false;
    hot_ = Part::None;
}

void MediaPanelWindow::SetContent(bool protectedVideo)
{
    if (protected_ == protectedVideo) {
        return;
    }
    protected_ = protectedVideo;
    if (window_ != nullptr) {
        ::InvalidateRect(window_, nullptr, FALSE);
    }
}

void MediaPanelWindow::SetMini(bool mini)
{
    if (mini_ == mini) {
        return;
    }
    mini_ = mini;
    if (window_ != nullptr && visible_) {
        RECT rect{};
        ::GetWindowRect(window_, &rect);
        const SIZE size = SizeForDpi(dpi_);
        // Keeps the right edge, where the panel sits over the video's corner.
        ::SetWindowPos(window_, HWND_TOPMOST, rect.right - size.cx, rect.top, size.cx, size.cy, SWP_NOACTIVATE);
        ::InvalidateRect(window_, nullptr, FALSE);
    }
}

void MediaPanelWindow::ThemeChanged()
{
    if (window_ != nullptr) {
        ::InvalidateRect(window_, nullptr, FALSE);
    }
}

void MediaPanelWindow::StartTimer(UINT intervalMs)
{
    if (window_ != nullptr && !timerRunning_) {
        timerRunning_ = ::SetTimer(window_, kTimerId, intervalMs, nullptr) != 0;
    }
}

void MediaPanelWindow::StopTimer()
{
    if (window_ != nullptr && timerRunning_) {
        ::KillTimer(window_, kTimerId);
    }
    timerRunning_ = false;
}

RECT MediaPanelWindow::PartRect(Part part) const
{
    RECT client{};
    ::GetClientRect(window_, &client);
    if (mini_) {
        return part == Part::Main ? client : RECT{};
    }
    const int side = Scale(kSideButton96, dpi_);
    const LONG mainRight = client.right - 2 * side;
    switch (part) {
    case Part::Main:
        return RECT{client.left, client.top, mainRight, client.bottom};
    case Part::Small:
        return RECT{mainRight, client.top, mainRight + side, client.bottom};
    case Part::Close:
        return RECT{mainRight + side, client.top, client.right, client.bottom};
    default:
        return RECT{};
    }
}

MediaPanelWindow::Part MediaPanelWindow::HitTest(POINT client) const
{
    for (const Part part : {Part::Main, Part::Small, Part::Close}) {
        const RECT rect = PartRect(part);
        if (::PtInRect(&rect, client)) {
            return part;
        }
    }
    return Part::None;
}

void MediaPanelWindow::SetHot(Part part)
{
    if (hot_ != part) {
        hot_ = part;
        ::InvalidateRect(window_, nullptr, FALSE);
    }
}

LRESULT CALLBACK MediaPanelWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<MediaPanelWindow*>(::GetWindowLongPtrW(window, GWLP_USERDATA));  // stored at WM_NCCREATE
    if (self == nullptr || (self->window_ != nullptr && self->window_ != window)) {
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
    if (self->window_ == nullptr) {
        self->window_ = window;
    }
    return self->HandleMessage(message, wParam, lParam);
}

LRESULT MediaPanelWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;  // clicks never take the focus from the browser
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
            tracking_ = ::TrackMouseEvent(&track) != FALSE;
        }
        SetHot(HitTest(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}));
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        SetHot(Part::None);
        return 0;
    case WM_LBUTTONUP: {
        const Part part = HitTest(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        if (part == Part::Main && callbacks_.openMenu) {
            RECT rect{};
            ::GetWindowRect(window_, &rect);
            callbacks_.openMenu(POINT{rect.left, rect.bottom});
        } else if (part == Part::Small) {
            SetMini(true);
        } else if (part == Part::Close && callbacks_.close) {
            callbacks_.close();
        }
        return 0;
    }
    case WM_TIMER:
        if (wParam == kTimerId && callbacks_.tick) {
            callbacks_.tick();
        }
        return 0;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    default:
        return ::DefWindowProcW(window_, message, wParam, lParam);
    }
}

void MediaPanelWindow::Paint()
{
    PAINTSTRUCT paint{};
    const HDC dc = ::BeginPaint(window_, &paint);
    RECT client{};
    ::GetClientRect(window_, &client);
    const Palette& colors = theme_->Colors();
    UniqueBrush background(::CreateSolidBrush(colors.panel));
    UniqueBrush hot(::CreateSolidBrush(colors.hot));
    UniqueBrush border(::CreateSolidBrush(colors.accent));
    ::FillRect(dc, &client, background.get());
    if (hot_ != Part::None) {
        const RECT hotRect = PartRect(hot_);
        ::FillRect(dc, &hotRect, hot.get());
    }
    ::FrameRect(dc, &client, border.get());

    const int padding = Scale(kPadding96, dpi_);
    const int iconSize = Scale(kIcon96, dpi_);
    const int gap = Scale(kGap96, dpi_);
    const int arrow = Scale(kArrow96, dpi_);
    const int middle = (client.top + client.bottom) / 2;
    ::DrawIconEx(dc, padding, middle - iconSize / 2, icon_.get(), iconSize, iconSize, 0, nullptr, DI_NORMAL);

    const RECT main = PartRect(Part::Main);
    int x = padding + iconSize + gap;
    if (!mini_) {
        const SelectObjectScope font(dc, font_.get());
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, colors.text);
        const std::wstring_view text = protected_ ? kProtectedText : kDownloadText;
        RECT textRect{x, client.top, main.right - padding - arrow - gap, client.bottom};
        ::DrawTextW(dc, text.data(), static_cast<int>(text.size()), &textRect,
                    DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
        x = main.right - padding - arrow;
    }

    // The menu arrow: a small filled triangle pointing down.
    UniqueBrush ink(::CreateSolidBrush(colors.text));
    UniquePen pen(::CreatePen(PS_SOLID, std::max(1, Scale(1, dpi_)), colors.text));
    {
        const SelectObjectScope selectBrush(dc, ink.get());
        const SelectObjectScope selectPen(dc, pen.get());
        const int half = arrow / 2;
        const POINT triangle[3] = {{x, middle - half / 2}, {x + arrow, middle - half / 2}, {x + half, middle + half / 2 + 1}};
        ::Polygon(dc, triangle, 3);
        if (!mini_) {
            const RECT shrink = PartRect(Part::Small);
            const RECT close = PartRect(Part::Close);
            const int mark = Scale(4, dpi_);
            const int shrinkMiddle = (shrink.left + shrink.right) / 2;
            DrawLine(dc, shrinkMiddle - mark, middle, shrinkMiddle + mark + 1, middle);
            const int closeMiddle = (close.left + close.right) / 2;
            DrawLine(dc, closeMiddle - mark, middle - mark, closeMiddle + mark + 1, middle + mark + 1);
            DrawLine(dc, closeMiddle + mark, middle - mark, closeMiddle - mark - 1, middle + mark + 1);
        }
    }
    ::EndPaint(window_, &paint);
}

}  // namespace ldm::ui
