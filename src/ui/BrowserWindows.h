#pragma once

#include "browser/Protocol.h"

#include <windows.h>

#include <cstdint>
#include <optional>

namespace ldm::ui {

// Where a browser shows a tab's page, in physical screen pixels.
struct BrowserView {
    HWND window = nullptr;  // the browser's top-level window
    RECT page{};            // the area the page is drawn in
};

// Finds the browser window a video position belongs to, for the media panel (M3 design section
// 6). The extension knows where the video is in the page; only Windows knows where the page is
// on screen. Implemented in platform/win. UI thread only.
class BrowserWindows {
public:
    virtual ~BrowserWindows() = default;

    // The window of the browser behind host process `hostProcessId` that `position` describes
    // (by its bounds and page size), or nothing: minimized, closed, or no window matches well
    // enough to place anything over it.
    //
    // `known` is the window this tab was found in before, or null. The reported bounds are only as
    // fresh as the extension's last report, so a window the user moves matches them badly until
    // the next one, and another browser window of the same size may match better. So `known` is
    // kept while it still shows a page of the reported size; with `recheck` (a new report just
    // came), another window replaces it only when it matches the report clearly better (the tab
    // was dragged to another window).
    [[nodiscard]] virtual std::optional<BrowserView> Find(std::uint32_t hostProcessId,
                                                          const browser::VideoPosition& position, HWND known,
                                                          bool recheck) = 0;

    // True when the user sees `view` at `point`: no other window covers it (LDM's own `panel`
    // does not count).
    [[nodiscard]] virtual bool IsShownAt(const BrowserView& view, POINT point, HWND panel) = 0;
};

}  // namespace ldm::ui
