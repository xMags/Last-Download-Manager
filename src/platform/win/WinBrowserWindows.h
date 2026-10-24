#pragma once

#include "ui/BrowserWindows.h"

#include <cstdint>
#include <map>

namespace ldm::platform {

// Finds browser windows and their page areas with Win32 (M3 design section 6).
//
// The browser is the process that started ldm-host.exe: Chrome and Edge start it through
// cmd.exe, Firefox directly. Among that process's top-level windows, the one whose bounds match
// what the extension reported is the tab's window. Its page area is, in Chrome and Edge, the
// render widget child window of the page's size; Firefox draws pages without a child window but
// tells the page's screen position itself (mozInnerScreenX), which the extension passes on.
class WinBrowserWindows final : public ui::BrowserWindows {
public:
    [[nodiscard]] std::optional<ui::BrowserView> Find(std::uint32_t hostProcessId,
                                                      const browser::VideoPosition& position) override;
    [[nodiscard]] bool IsShownAt(const ui::BrowserView& view, POINT point, HWND panel) override;

private:
    [[nodiscard]] std::uint32_t BrowserProcessOf(std::uint32_t hostProcessId);

    std::map<std::uint32_t, std::uint32_t> browsers_;  // host process -> browser process
};

}  // namespace ldm::platform
