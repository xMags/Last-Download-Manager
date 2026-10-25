#include "ui/MediaPanelWindow.h"

#include "ui/Dpi.h"
#include "ui/GlyphIcons.h"
#include "ui/MediaPanelMenuPainter.h"

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <string_view>

namespace ldm::ui {

namespace {

constexpr wchar_t kClassName[] = L"LdmMediaPanel";
// Icon font glyphs (GlyphIcons).
constexpr wchar_t kDownloadGlyph = 0xE896;     // Download
constexpr wchar_t kLockGlyph = 0xE72E;         // Lock
constexpr wchar_t kChevronDownGlyph = 0xE70D;  // ChevronDown
constexpr wchar_t kMinimizeGlyph = 0xE921;     // ChromeMinimize
constexpr wchar_t kCloseGlyph = 0xE8BB;        // ChromeClose
constexpr UINT_PTR kTimerId = 1;
constexpr std::wstring_view kDownloadText = L"Download this video";
constexpr std::wstring_view kProtectedText = L"Protected video";

// Sizes at 96 DPI.
constexpr int kHeight96 = 32;
constexpr int kPadding96 = 10;
constexpr int kIcon96 = 16;
constexpr int kGap96 = 8;
constexpr int kArrow96 = 10;
constexpr int kSideButton96 = 30;
constexpr int kDividerInset96 = 8;
constexpr int kTextSlack96 = 4;

// The panel sits on a web video, not in LDM's window, so it has one look in light and dark
// mode: a blue strong enough for white text (contrast about 5:1), or slate for a protected video
// that offers nothing to download.
constexpr COLORREF kBlue = RGB(0x1F, 0x6F, 0xD1);
constexpr COLORREF kBlueBorder = RGB(0x17, 0x58, 0xA8);
constexpr COLORREF kSlate = RGB(0x4A, 0x55, 0x63);
constexpr COLORREF kSlateBorder = RGB(0x3A, 0x43, 0x4F);
constexpr COLORREF kInk = RGB(0xFF, 0xFF, 0xFF);

COLORREF Blend(COLORREF from, COLORREF to, int percent)
{
    const auto mix = [percent](int a, int b) { return a + (b - a) * percent / 100; };
    return RGB(mix(GetRValue(from), GetRValue(to)), mix(GetGValue(from), GetGValue(to)),
               mix(GetBValue(from), GetBValue(to)));
}

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

// The message font in semibold, for the button's text.
UniqueFont CreateLabelFont(UINT dpi)
{
    const UniqueFont message = CreateMessageFont(dpi);
    LOGFONTW font{};
    if (::GetObjectW(message.get(), sizeof(font), &font) == 0) {
        return CreateMessageFont(dpi);
    }
    font.lfWeight = FW_SEMIBOLD;
    return UniqueFont(::CreateFontIndirectW(&font));
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

void DrawGlyph(HDC dc, HFONT font, wchar_t glyph, RECT rect)
{
    const SelectObjectScope select(dc, font);
    ::DrawTextW(dc, &glyph, 1, &rect, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
}

}  // namespace

MediaPanelWindow::~MediaPanelWindow()
{
    Destroy();
}

bool MediaPanelWindow::Create(Callbacks callbacks)
{
    if (window_ != nullptr) {
        return true;
    }
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
    // Windows 11 rounds the corners with smooth edges; older Windows ignores both attributes.
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    ::DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    const COLORREF border = kBlueBorder;
    ::DwmSetWindowAttribute(window_, DWMWA_BORDER_COLOR, &border, sizeof(border));
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
    font_ = CreateLabelFont(dpi);
    glyphs_ = CreateIconFont(Scale(kIcon96, dpi));
    smallGlyphs_ = CreateIconFont(Scale(kArrow96, dpi));
}

SIZE MediaPanelWindow::SizeForDpi(UINT dpi) const
{
    const int height = Scale(kHeight96, dpi);
    if (mini_) {
        return SIZE{Scale(kPadding96 + kIcon96 + kGap96 + kArrow96 + kPadding96, dpi), height};
    }
    const std::wstring_view text = protected_ ? kProtectedText : kDownloadText;
    const UniqueFont measuring = dpi == dpi_ ? UniqueFont() : CreateLabelFont(dpi);
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
    pressed_ = Part::None;
}

void MediaPanelWindow::SetContent(bool protectedVideo)
{
    if (protected_ == protectedVideo) {
        return;
    }
    protected_ = protectedVideo;
    if (window_ != nullptr) {
        const COLORREF border = protected_ ? kSlateBorder : kBlueBorder;
        ::DwmSetWindowAttribute(window_, DWMWA_BORDER_COLOR, &border, sizeof(border));
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

void MediaPanelWindow::SetPressed(Part part)
{
    if (pressed_ != part) {
        pressed_ = part;
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
    case WM_LBUTTONDOWN:
        // Captured, so a release outside the panel is seen too (and does nothing).
        SetPressed(HitTest(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}));
        ::SetCapture(window_);
        return 0;
    case WM_CAPTURECHANGED:
        SetPressed(Part::None);
        return 0;
    case WM_LBUTTONUP: {
        const Part pressed = pressed_;
        // Released before acting: the menu runs its own loop and needs the mouse.
        ::ReleaseCapture();
        SetPressed(Part::None);
        const Part part = HitTest(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        if (part != pressed) {
            return 0;
        }
        if (part == Part::Main && callbacks_.openMenu) {
            RECT rect{};
            ::GetWindowRect(window_, &rect);
            callbacks_.openMenu(POINT{rect.left, rect.bottom + Scale(2, dpi_)});
        } else if (part == Part::Small) {
            SetMini(true);
        } else if (part == Part::Close && callbacks_.close) {
            callbacks_.close();
        }
        return 0;
    }
    case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);  // the message's documented type
        if (menuPainter_ != nullptr && measure->CtlType == ODT_MENU) {
            menuPainter_->Measure(*measure);
            return TRUE;
        }
        return ::DefWindowProcW(window_, message, wParam, lParam);
    }
    case WM_DRAWITEM: {
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);  // the message's documented type
        if (menuPainter_ != nullptr && draw->CtlType == ODT_MENU) {
            menuPainter_->Draw(*draw);
            return TRUE;
        }
        return ::DefWindowProcW(window_, message, wParam, lParam);
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
    const HDC windowDc = ::BeginPaint(window_, &paint);
    RECT client{};
    ::GetClientRect(window_, &client);
    // Drawn off screen first, so hovering never shows a half-painted panel.
    const HDC dc = ::CreateCompatibleDC(windowDc);
    UniqueBitmap bitmap(::CreateCompatibleBitmap(windowDc, client.right, client.bottom));
    if (dc == nullptr || !bitmap) {
        if (dc != nullptr) {
            ::DeleteDC(dc);
        }
        ::EndPaint(window_, &paint);
        return;
    }
    {
        const SelectObjectScope selectBitmap(dc, bitmap.get());
        const COLORREF base = protected_ ? kSlate : kBlue;
        UniqueBrush background(::CreateSolidBrush(base));
        ::FillRect(dc, &client, background.get());
        for (const Part part : {Part::Main, Part::Small, Part::Close}) {
            if (part != hot_ && part != pressed_) {
                continue;
            }
            // Pressed darkens; under the mouse lightens. The rounded window corners clip both.
            const COLORREF fill = part == pressed_ ? Blend(base, RGB(0, 0, 0), 18) : Blend(base, kInk, 14);
            UniqueBrush brush(::CreateSolidBrush(fill));
            const RECT rect = PartRect(part);
            ::FillRect(dc, &rect, brush.get());
        }

        const int padding = Scale(kPadding96, dpi_);
        const int iconSize = Scale(kIcon96, dpi_);
        const int gap = Scale(kGap96, dpi_);
        const int arrow = Scale(kArrow96, dpi_);
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, kInk);
        DrawGlyph(dc, glyphs_.get(), protected_ ? kLockGlyph : kDownloadGlyph,
                  RECT{padding, client.top, padding + iconSize, client.bottom});

        const RECT main = PartRect(Part::Main);
        int x = padding + iconSize + gap;
        if (!mini_) {
            const SelectObjectScope font(dc, font_.get());
            const std::wstring_view text = protected_ ? kProtectedText : kDownloadText;
            RECT textRect{x, client.top, main.right - padding - arrow - gap, client.bottom};
            ::DrawTextW(dc, text.data(), static_cast<int>(text.size()), &textRect,
                        DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
            x = main.right - padding - arrow;
        }
        DrawGlyph(dc, smallGlyphs_.get(), kChevronDownGlyph, RECT{x, client.top, x + arrow, client.bottom});

        if (!mini_) {
            // A faint line between the button and the two small ones, unless one of them is lit.
            if (hot_ == Part::None && pressed_ == Part::None) {
                const int inset = Scale(kDividerInset96, dpi_);
                const RECT rule{main.right, client.top + inset, main.right + std::max(1, Scale(1, dpi_)),
                                client.bottom - inset};
                UniqueBrush line(::CreateSolidBrush(Blend(base, kInk, 35)));
                ::FillRect(dc, &rule, line.get());
            }
            DrawGlyph(dc, smallGlyphs_.get(), kMinimizeGlyph, PartRect(Part::Small));
            DrawGlyph(dc, smallGlyphs_.get(), kCloseGlyph, PartRect(Part::Close));
        }
        ::BitBlt(windowDc, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    }
    ::DeleteDC(dc);
    ::EndPaint(window_, &paint);
}

}  // namespace ldm::ui
