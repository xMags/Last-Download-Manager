#include "platform/win/WinBrowserWindows.h"

#include <windows.h>
#include <dwmapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace ldm::platform {

namespace {

constexpr wchar_t kChromiumWindowClass[] = L"Chrome_WidgetWin_1";
constexpr wchar_t kChromiumPageClass[] = L"Chrome_RenderWidgetHostHWND";
constexpr wchar_t kFirefoxWindowClass[] = L"MozillaWindowClass";
// Processes between the browser and ldm-host.exe that are not the browser.
constexpr const wchar_t* kLaunchers[] = {L"cmd.exe", L"conhost.exe", L"ldm-host.exe"};
// How far the found page size may be from the reported one, in pixels: rounding of fractional
// pixel ratios.
constexpr double kPageSizeTolerance = 3;

std::wstring ClassOf(HWND window)
{
    wchar_t name[64] = {};
    const int length = ::GetClassNameW(window, name, static_cast<int>(std::size(name)));
    return std::wstring(name, length > 0 ? static_cast<size_t>(length) : 0);
}

std::wstring Lower(std::wstring text)
{
    for (wchar_t& c : text) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return text;
}

struct ProcessEntry {
    std::uint32_t parent = 0;
    std::wstring exe;  // lower case
};

std::map<std::uint32_t, ProcessEntry> ProcessTable()
{
    std::map<std::uint32_t, ProcessEntry> table;
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return table;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = ::Process32FirstW(snapshot, &entry); more; more = ::Process32NextW(snapshot, &entry)) {
        table[entry.th32ProcessID] = ProcessEntry{entry.th32ParentProcessID, Lower(entry.szExeFile)};
    }
    ::CloseHandle(snapshot);
    return table;
}

struct Candidate {
    HWND window = nullptr;
    double distance = std::numeric_limits<double>::max();
};

struct WindowSearch {
    std::uint32_t processId = 0;
    const browser::VideoPosition* position = nullptr;
    Candidate best;
};

BOOL CALLBACK VisitTopWindow(HWND window, LPARAM parameter)
{
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);  // EnumWindows hands back our pointer
    DWORD owner = 0;
    ::GetWindowThreadProcessId(window, &owner);
    if (owner != search.processId || !::IsWindowVisible(window) || ::IsIconic(window)) {
        return TRUE;
    }
    const std::wstring windowClass = ClassOf(window);
    if (windowClass != kChromiumWindowClass && windowClass != kFirefoxWindowClass) {
        return TRUE;
    }
    if (::GetWindowTextLengthW(window) == 0) {
        return TRUE;  // menus, tooltips and other helper windows have no title
    }
    RECT bounds{};
    if (!::GetWindowRect(window, &bounds)) {
        return TRUE;
    }
    // The extension reports the window in CSS pixels of the screen: physical pixels at the
    // window's display scale.
    const double scale = static_cast<double>(::GetDpiForWindow(window)) / USER_DEFAULT_SCREEN_DPI;
    const browser::CssRect& reported = search.position->window;
    const double distance = std::abs(bounds.left - reported.x * scale) + std::abs(bounds.top - reported.y * scale) +
                            std::abs((bounds.right - bounds.left) - reported.width * scale) +
                            std::abs((bounds.bottom - bounds.top) - reported.height * scale);
    if (distance < search.best.distance) {
        search.best = Candidate{window, distance};
    }
    return TRUE;
}

struct PageSearch {
    double width = 0;   // expected, physical pixels
    double height = 0;
    HWND best = nullptr;
    double distance = std::numeric_limits<double>::max();
};

BOOL CALLBACK VisitChild(HWND child, LPARAM parameter)
{
    auto& search = *reinterpret_cast<PageSearch*>(parameter);  // EnumChildWindows hands back our pointer
    if (!::IsWindowVisible(child) || ClassOf(child) != kChromiumPageClass) {
        return TRUE;
    }
    RECT rect{};
    if (!::GetWindowRect(child, &rect)) {
        return TRUE;
    }
    const double dw = std::abs((rect.right - rect.left) - search.width);
    const double dh = std::abs((rect.bottom - rect.top) - search.height);
    if (dw <= kPageSizeTolerance && dh <= kPageSizeTolerance && dw + dh < search.distance) {
        search.best = child;
        search.distance = dw + dh;
    }
    return TRUE;
}

}  // namespace

std::uint32_t WinBrowserWindows::BrowserProcessOf(std::uint32_t hostProcessId)
{
    if (const auto known = browsers_.find(hostProcessId); known != browsers_.end()) {
        return known->second;
    }
    const auto table = ProcessTable();
    std::uint32_t current = hostProcessId;
    for (int depth = 0; depth < 4; ++depth) {
        const auto entry = table.find(current);
        if (entry == table.end()) {
            return 0;
        }
        const auto parent = table.find(entry->second.parent);
        if (parent == table.end()) {
            return 0;  // the parent ended: the id may be reused, so it is not trusted
        }
        const bool launcher = std::any_of(std::begin(kLaunchers), std::end(kLaunchers),
                                          [&parent](const wchar_t* name) { return parent->second.exe == name; });
        if (!launcher) {
            browsers_[hostProcessId] = parent->first;
            return parent->first;
        }
        current = parent->first;
    }
    return 0;
}

std::optional<ui::BrowserView> WinBrowserWindows::Find(std::uint32_t hostProcessId, const browser::VideoPosition& position)
{
    const std::uint32_t browser = hostProcessId != 0 ? BrowserProcessOf(hostProcessId) : 0;
    if (browser == 0) {
        return std::nullopt;
    }
    WindowSearch windows;
    windows.processId = browser;
    windows.position = &position;
    ::EnumWindows(&VisitTopWindow, reinterpret_cast<LPARAM>(&windows));  // the callback reads it back
    if (windows.best.window == nullptr) {
        return std::nullopt;
    }
    ui::BrowserView view;
    view.window = windows.best.window;
    const double pageWidth = position.viewport.width * position.pixelRatio;
    const double pageHeight = position.viewport.height * position.pixelRatio;
    if (ClassOf(view.window) == kChromiumWindowClass) {
        PageSearch page;
        page.width = pageWidth;
        page.height = pageHeight;
        ::EnumChildWindows(view.window, &VisitChild, reinterpret_cast<LPARAM>(&page));  // read back in VisitChild
        if (page.best == nullptr || !::GetWindowRect(page.best, &view.page)) {
            return std::nullopt;
        }
        return view;
    }
    if (!position.viewportPositionKnown) {
        return std::nullopt;
    }
    const LONG left = std::lround(position.viewport.x * position.pixelRatio);
    const LONG top = std::lround(position.viewport.y * position.pixelRatio);
    view.page = RECT{left, top, left + std::lround(pageWidth), top + std::lround(pageHeight)};
    return view;
}

bool WinBrowserWindows::IsShownAt(const ui::BrowserView& view, POINT point, HWND panel)
{
    // The topmost visible window at the point, other than the panel itself. Not the foreground
    // window: the browser may be what the user looks at while another program has the keyboard.
    for (HWND window = ::GetTopWindow(nullptr); window != nullptr; window = ::GetWindow(window, GW_HWNDNEXT)) {
        if (window == panel || !::IsWindowVisible(window) || ::IsIconic(window)) {
            continue;
        }
        BOOL cloaked = FALSE;  // windows of other virtual desktops and suspended apps are "visible" but not shown
        if (SUCCEEDED(::DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
            continue;
        }
        if ((::GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0) {
            continue;  // overlays that let clicks through, such as screen recorders' frames
        }
        RECT rect{};
        if (!::GetWindowRect(window, &rect) || !::PtInRect(&rect, point)) {
            continue;
        }
        return window == view.window;
    }
    return false;
}

}  // namespace ldm::platform
