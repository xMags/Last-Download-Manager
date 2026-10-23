#include "ui/Theme.h"

#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>

#include <string>

namespace ldm::ui {

namespace {

// Builds where the uxtheme ordinals below have the signatures used here. Older builds stay light.
constexpr DWORD kFirstDarkModeBuild = 18362;  // Windows 10 1903

enum class PreferredAppMode {
    Default,
    AllowDark,
    ForceDark,
    ForceLight,
};

using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
using AllowDarkModeForWindowFn = bool(WINAPI*)(HWND, bool);
using RefreshImmersiveColorPolicyStateFn = void(WINAPI*)();
using FlushMenuThemesFn = void(WINAPI*)();

// The undocumented uxtheme entry points, resolved once. All null when unavailable.
struct DarkModeApi {
    SetPreferredAppModeFn setPreferredAppMode = nullptr;
    AllowDarkModeForWindowFn allowDarkModeForWindow = nullptr;
    RefreshImmersiveColorPolicyStateFn refreshImmersiveColorPolicyState = nullptr;
    FlushMenuThemesFn flushMenuThemes = nullptr;

    [[nodiscard]] bool Available() const noexcept
    {
        return setPreferredAppMode != nullptr && allowDarkModeForWindow != nullptr &&
               refreshImmersiveColorPolicyState != nullptr && flushMenuThemes != nullptr;
    }
};

DWORD WindowsBuild()
{
    using RtlGetNtVersionNumbersFn = void(WINAPI*)(LPDWORD, LPDWORD, LPDWORD);
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return 0;
    }
    const auto getVersion =
        reinterpret_cast<RtlGetNtVersionNumbersFn>(::GetProcAddress(ntdll, "RtlGetNtVersionNumbers"));  // FARPROC to its real type
    if (getVersion == nullptr) {
        return 0;
    }
    DWORD major = 0;
    DWORD minor = 0;
    DWORD build = 0;
    getVersion(&major, &minor, &build);
    return build & 0x0FFFFFFF;  // the high bits flag free/checked builds
}

const DarkModeApi& Api()
{
    static const DarkModeApi api = [] {
        DarkModeApi result;
        if (WindowsBuild() < kFirstDarkModeBuild) {
            return result;
        }
        const HMODULE uxtheme = ::LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (uxtheme == nullptr) {
            return result;
        }
        // Ordinals: 104 RefreshImmersiveColorPolicyState, 133 AllowDarkModeForWindow,
        // 135 SetPreferredAppMode, 136 FlushMenuThemes. Casts from FARPROC are required by
        // GetProcAddress's untyped return.
        result.refreshImmersiveColorPolicyState = reinterpret_cast<RefreshImmersiveColorPolicyStateFn>(
            ::GetProcAddress(uxtheme, MAKEINTRESOURCEA(104)));
        result.allowDarkModeForWindow =
            reinterpret_cast<AllowDarkModeForWindowFn>(::GetProcAddress(uxtheme, MAKEINTRESOURCEA(133)));
        result.setPreferredAppMode =
            reinterpret_cast<SetPreferredAppModeFn>(::GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
        result.flushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(::GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
        if (!result.Available()) {
            return DarkModeApi{};
        }
        return result;
    }();
    return api;
}

bool WindowsAppsUseDarkMode()
{
    DWORD value = 1;
    DWORD size = sizeof(value);
    const LSTATUS status =
        ::RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)",
                       L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return status == ERROR_SUCCESS && value == 0;
}

bool HighContrast()
{
    HIGHCONTRASTW contrast{sizeof(contrast)};
    return ::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
           (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

// Undocumented menu bar drawing messages and structures (stable since Windows 10 1809).
constexpr UINT kUahDrawMenu = 0x0091;
constexpr UINT kUahDrawMenuItem = 0x0092;

struct UahMenu {
    HMENU menu;
    HDC dc;
    DWORD flags;
};
union UahMenuItemMetrics {
    struct {
        DWORD cx;
        DWORD cy;
    } rgsizeBar[2];
    struct {
        DWORD cx;
        DWORD cy;
    } rgsizePopup[4];
};
struct UahMenuPopupMetrics {
    DWORD rgcx[4];
    DWORD fUpdateMaxWidths : 2;
};
struct UahMenuItem {
    int position;
    UahMenuItemMetrics metrics;
    UahMenuPopupMetrics popupMetrics;
};
struct UahDrawMenuItem {
    DRAWITEMSTRUCT draw;
    UahMenu menu;
    UahMenuItem item;
};

}  // namespace

void Theme::InitializeProcess()
{
    const DarkModeApi& api = Api();
    if (!api.Available()) {
        return;
    }
    api.setPreferredAppMode(PreferredAppMode::AllowDark);
    api.refreshImmersiveColorPolicyState();
    api.flushMenuThemes();
}

Theme::Theme(bool settingEnabled)
{
    Refresh(settingEnabled);
    if (!windowBrush_) {
        BuildPalette();
    }
}

bool Theme::Refresh(bool settingEnabled)
{
    const bool dark = settingEnabled && Api().Available() && WindowsAppsUseDarkMode() && !HighContrast();
    const bool changed = dark != dark_ || !windowBrush_;
    dark_ = dark;
    if (changed) {
        if (Api().Available()) {
            Api().setPreferredAppMode(dark_ ? PreferredAppMode::ForceDark : PreferredAppMode::ForceLight);
            Api().refreshImmersiveColorPolicyState();
            Api().flushMenuThemes();
        }
        BuildPalette();
    }
    return changed;
}

void Theme::BuildPalette()
{
    if (dark_) {
        palette_ = Palette{RGB(0x20, 0x20, 0x20), RGB(0x2B, 0x2B, 0x2B), RGB(0xF0, 0xF0, 0xF0), RGB(0xA0, 0xA0, 0xA0),
                           RGB(0x40, 0x40, 0x40), RGB(0x41, 0x41, 0x41), RGB(0x3C, 0x9C, 0xE8), RGB(0xFF, 0xFF, 0xFF),
                           RGB(0x3A, 0x3A, 0x3A)};
    } else {
        palette_ = Palette{::GetSysColor(COLOR_WINDOW),    ::GetSysColor(COLOR_BTNFACE), ::GetSysColor(COLOR_WINDOWTEXT),
                           ::GetSysColor(COLOR_GRAYTEXT),  ::GetSysColor(COLOR_3DSHADOW), RGB(0xE0, 0xE0, 0xE0),
                           RGB(0x2D, 0x7D, 0xD2), RGB(0xFF, 0xFF, 0xFF), RGB(0xE0, 0xE0, 0xE0)};
    }
    windowBrush_.reset(::CreateSolidBrush(palette_.window));
    panelBrush_.reset(::CreateSolidBrush(palette_.panel));
    hotBrush_.reset(::CreateSolidBrush(palette_.hot));
}

void Theme::ApplyToTopLevel(HWND window) const
{
    const BOOL dark = dark_ ? TRUE : FALSE;
    ::DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    // Windows 11 only; earlier builds reject the attribute and keep their own caption color.
    const COLORREF caption = HighContrast() ? DWMWA_COLOR_DEFAULT : palette_.panel;
    ::DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    if (Api().Available()) {
        Api().allowDarkModeForWindow(window, dark_);
    }
}

void Theme::ApplyToControl(HWND control) const
{
    wchar_t className[64] = {};
    ::GetClassNameW(control, className, static_cast<int>(std::size(className)));
    const std::wstring_view name(className);
    if (Api().Available()) {
        Api().allowDarkModeForWindow(control, dark_);
    }
    if (name == WC_HEADERW) {
        ::SetWindowTheme(control, dark_ ? L"DarkMode_ItemsView" : L"ItemsView", nullptr);
        return;
    }
    if (name == WC_COMBOBOXW) {
        ::SetWindowTheme(control, dark_ ? L"DarkMode_CFD" : L"CFD", nullptr);
        return;
    }
    if (name == WC_LISTVIEWW || name == WC_TREEVIEWW) {
        ::SetWindowTheme(control, dark_ ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        return;
    }
    ::SetWindowTheme(control, dark_ ? L"DarkMode_Explorer" : nullptr, nullptr);
    ::SendMessageW(control, WM_THEMECHANGED, 0, 0);
}

void Theme::ApplyToChildren(HWND parent) const
{
    ::EnumChildWindows(
        parent,
        [](HWND child, LPARAM context) -> BOOL {
            reinterpret_cast<const Theme*>(context)->ApplyToControl(child);  // LPARAM carries `this`
            return TRUE;
        },
        reinterpret_cast<LPARAM>(this));
}

HBRUSH Theme::OnCtlColor(HDC dc, bool editable) const
{
    ::SetTextColor(dc, palette_.text);
    ::SetBkColor(dc, editable ? palette_.window : palette_.panel);
    return editable ? windowBrush_.get() : panelBrush_.get();
}

bool Theme::HandleMenuBarMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) const
{
    if (HighContrast()) {
        return false;  // high contrast keeps the system's own menu bar colors
    }
    switch (message) {
    case kUahDrawMenu: {
        const auto* menu = reinterpret_cast<const UahMenu*>(lParam);  // documented by convention only
        MENUBARINFO info{sizeof(info)};
        if (!::GetMenuBarInfo(window, OBJID_MENU, 0, &info)) {
            return false;
        }
        RECT windowRect{};
        ::GetWindowRect(window, &windowRect);
        RECT bar = info.rcBar;
        ::OffsetRect(&bar, -windowRect.left, -windowRect.top);
        ::FillRect(menu->dc, &bar, panelBrush_.get());
        result = TRUE;
        return true;
    }
    case kUahDrawMenuItem: {
        auto* item = reinterpret_cast<UahDrawMenuItem*>(lParam);
        wchar_t text[256] = {};
        MENUITEMINFOW info{sizeof(info)};
        info.fMask = MIIM_STRING;
        info.dwTypeData = text;
        info.cch = static_cast<UINT>(std::size(text) - 1);
        ::GetMenuItemInfoW(item->menu.menu, static_cast<UINT>(item->item.position), TRUE, &info);

        const UINT state = item->draw.itemState;
        const bool hot = (state & (ODS_HOTLIGHT | ODS_SELECTED)) != 0;
        const bool disabled = (state & (ODS_INACTIVE | ODS_DISABLED | ODS_GRAYED)) != 0;
        ::FillRect(item->menu.dc, &item->draw.rcItem, hot ? hotBrush_.get() : panelBrush_.get());
        DWORD flags = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
        if ((state & ODS_NOACCEL) != 0) {
            flags |= DT_HIDEPREFIX;
        }
        ::SetBkMode(item->menu.dc, TRANSPARENT);
        ::SetTextColor(item->menu.dc, disabled ? palette_.mutedText : palette_.text);
        ::DrawTextW(item->menu.dc, text, -1, &item->draw.rcItem, flags);
        result = TRUE;
        return true;
    }
    case WM_NCPAINT:
    case WM_NCACTIVATE: {
        // Let Windows paint the frame, then cover the light line it draws under the menu bar.
        result = ::DefWindowProcW(window, message, wParam, lParam);
        MENUBARINFO info{sizeof(info)};
        if (!::GetMenuBarInfo(window, OBJID_MENU, 0, &info)) {
            return true;
        }
        RECT client{};
        ::GetClientRect(window, &client);
        ::MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&client), 2);
        RECT windowRect{};
        ::GetWindowRect(window, &windowRect);
        ::OffsetRect(&client, -windowRect.left, -windowRect.top);
        RECT line = client;
        line.bottom = line.top;
        line.top -= 1;
        const HDC dc = ::GetWindowDC(window);
        ::FillRect(dc, &line, panelBrush_.get());
        ::ReleaseDC(window, dc);
        return true;
    }
    default:
        return false;
    }
}

}  // namespace ldm::ui
