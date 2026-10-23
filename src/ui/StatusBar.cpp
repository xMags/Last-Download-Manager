#include "ui/StatusBar.h"

#include "ui/Dpi.h"

namespace ldm::ui {

namespace {

constexpr wchar_t kClassName[] = L"LdmStatusBar";

}  // namespace

void StatusBar::RegisterClassOnce()
{
    static const ATOM atom = [] {
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &StatusBar::WindowProc;
        windowClass.hInstance = ::GetModuleHandleW(nullptr);
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kClassName;
        return ::RegisterClassExW(&windowClass);
    }();
    (void)atom;
}

StatusBar::~StatusBar()
{
    if (window_ != nullptr && ::IsWindow(window_)) {
        ::SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
    }
}

void StatusBar::Create(HWND parent, const Theme& theme)
{
    RegisterClassOnce();
    theme_ = &theme;
    window_ = ::CreateWindowExW(0, kClassName, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent, nullptr,
                                ::GetModuleHandleW(nullptr), this);
}

int StatusBar::PreferredHeight() const
{
    return Scale(24, DpiOf(window_));
}

void StatusBar::SetTheme(const Theme& theme)
{
    theme_ = &theme;
    ::InvalidateRect(window_, nullptr, TRUE);
}

void StatusBar::SetFont(HFONT font)
{
    font_ = font;
    ::InvalidateRect(window_, nullptr, TRUE);
}

void StatusBar::SetParts(std::vector<std::wstring> parts)
{
    if (parts == parts_) {
        return;
    }
    parts_ = std::move(parts);
    ::InvalidateRect(window_, nullptr, TRUE);
}

LRESULT CALLBACK StatusBar::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<StatusBar*>(::GetWindowLongPtrW(window, GWLP_USERDATA));  // stored at WM_NCCREATE
    if (self != nullptr) {
        switch (message) {
        case WM_PAINT:
            self->Paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        default:
            break;
        }
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

void StatusBar::Paint()
{
    PAINTSTRUCT paint{};
    const HDC dc = ::BeginPaint(window_, &paint);
    RECT client{};
    ::GetClientRect(window_, &client);
    const Palette& colors = theme_->Colors();
    ::FillRect(dc, &client, theme_->PanelBrush());

    RECT line = client;
    line.bottom = line.top + 1;
    UniqueBrush border(::CreateSolidBrush(colors.divider));
    ::FillRect(dc, &line, border.get());

    const UINT dpi = DpiOf(window_);
    const int padding = Scale(8, dpi);
    const HGDIOBJ previousFont = font_ != nullptr ? ::SelectObject(dc, font_) : nullptr;
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, colors.text);
    int x = client.left + padding;
    for (size_t i = 0; i < parts_.size(); ++i) {
        RECT measure{0, 0, 0, 0};
        ::DrawTextW(dc, parts_[i].c_str(), -1, &measure, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
        RECT text{x, client.top + 1, x + (measure.right - measure.left), client.bottom};
        ::DrawTextW(dc, parts_[i].c_str(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        x = text.right + padding;
        if (i + 1 < parts_.size()) {
            RECT divider{x, client.top + Scale(5, dpi), x + 1, client.bottom - Scale(4, dpi)};
            ::FillRect(dc, &divider, border.get());
            x += 1 + padding;
        }
    }
    if (previousFont != nullptr) {
        ::SelectObject(dc, previousFont);
    }
    ::EndPaint(window_, &paint);
}

}  // namespace ldm::ui
