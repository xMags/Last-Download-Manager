#include "ui/TrayIcon.h"

#include "resource.h"

#include <shellapi.h>

#include <vector>

namespace ldm::ui {

namespace {

constexpr UINT kIconId = 1;

UniqueIcon LoadTrayIcon(model::TrayIconStyle style)
{
    const int size = ::GetSystemMetrics(SM_CXSMICON);
    HICON icon = nullptr;
    ::LoadIconWithScaleDown(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_LDM), size, size, &icon);
    if (style != model::TrayIconStyle::Classic || icon == nullptr) {
        return UniqueIcon(icon);
    }
    // Classic style: the same artwork in grayscale, closer to the plain look of older trays.
    ICONINFO info{};
    if (!::GetIconInfo(icon, &info)) {
        return UniqueIcon(icon);
    }
    UniqueBitmap color(info.hbmColor);
    UniqueBitmap mask(info.hbmMask);
    BITMAP bitmap{};
    ::GetObjectW(color.get(), sizeof(bitmap), &bitmap);
    BITMAPINFO format{};
    format.bmiHeader.biSize = sizeof(format.bmiHeader);
    format.bmiHeader.biWidth = bitmap.bmWidth;
    format.bmiHeader.biHeight = -bitmap.bmHeight;
    format.bmiHeader.biPlanes = 1;
    format.bmiHeader.biBitCount = 32;
    std::vector<DWORD> pixels(static_cast<size_t>(bitmap.bmWidth) * static_cast<size_t>(bitmap.bmHeight), 0);
    const HDC screen = ::GetDC(nullptr);
    ::GetDIBits(screen, color.get(), 0, static_cast<UINT>(bitmap.bmHeight), pixels.data(), &format, DIB_RGB_COLORS);
    for (DWORD& pixel : pixels) {
        const DWORD alpha = pixel >> 24;
        const DWORD gray = (((pixel >> 16) & 0xFF) * 30 + ((pixel >> 8) & 0xFF) * 59 + (pixel & 0xFF) * 11) / 100;
        pixel = (alpha << 24) | (gray << 16) | (gray << 8) | gray;
    }
    ::SetDIBits(screen, color.get(), 0, static_cast<UINT>(bitmap.bmHeight), pixels.data(), &format, DIB_RGB_COLORS);
    ::ReleaseDC(nullptr, screen);
    ::DestroyIcon(icon);
    ICONINFO grayInfo{TRUE, 0, 0, mask.get(), color.get()};
    return UniqueIcon(::CreateIconIndirect(&grayInfo));
}

}  // namespace

TrayIcon::~TrayIcon()
{
    Remove();
}

UINT TrayIcon::TaskbarCreatedMessage()
{
    static const UINT message = ::RegisterWindowMessageW(L"TaskbarCreated");
    return message;
}

void TrayIcon::Attach(HWND owner)
{
    owner_ = owner;
    icon_ = LoadTrayIcon(style_);
    if (style_ != model::TrayIconStyle::Hidden) {
        Add();
    }
}

void TrayIcon::SetStyle(model::TrayIconStyle style)
{
    if (style == style_ && (added_ || style == model::TrayIconStyle::Hidden)) {
        return;
    }
    style_ = style;
    if (owner_ == nullptr) {
        return;
    }
    icon_ = LoadTrayIcon(style_);
    if (style_ == model::TrayIconStyle::Hidden) {
        Remove();
    } else if (added_) {
        Update();
    } else {
        Add();
    }
}

void TrayIcon::SetTooltip(const std::wstring& tooltip)
{
    if (tooltip == tooltip_) {
        return;
    }
    tooltip_ = tooltip;
    if (added_) {
        Update();
    }
}

void TrayIcon::Recreate()
{
    added_ = false;
    if (owner_ != nullptr && style_ != model::TrayIconStyle::Hidden) {
        Add();
    }
}

void TrayIcon::Add()
{
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = owner_;
    data.uID = kIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = icon_.get();
    wcsncpy_s(data.szTip, tooltip_.c_str(), _TRUNCATE);
    added_ = ::Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    if (added_) {
        data.uVersion = NOTIFYICON_VERSION_4;
        ::Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
}

void TrayIcon::Update()
{
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = owner_;
    data.uID = kIconId;
    data.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.hIcon = icon_.get();
    wcsncpy_s(data.szTip, tooltip_.c_str(), _TRUNCATE);
    ::Shell_NotifyIconW(NIM_MODIFY, &data);
}

void TrayIcon::Remove()
{
    if (!added_) {
        return;
    }
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = owner_;
    data.uID = kIconId;
    ::Shell_NotifyIconW(NIM_DELETE, &data);
    added_ = false;
}

}  // namespace ldm::ui
