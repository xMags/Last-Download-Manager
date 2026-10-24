#pragma once

#include "browser/Protocol.h"

#include <windows.h>

#include <optional>

// Where the media panel goes (M3 design section 6). Pure geometry in physical pixels.
namespace ldm::ui {

// The video's rectangle on screen: the page's origin plus the video's CSS rectangle scaled by
// the page's pixel ratio, clipped to the page. Nothing when less than `minimum` of it shows.
[[nodiscard]] std::optional<RECT> VideoOnScreen(const RECT& page, const browser::CssRect& video, double pixelRatio,
                                                SIZE minimum);

// The panel's top-left corner: at the video's top-right corner, just above the video when the
// page has room there (as the reference places it), otherwise just inside the video's top edge; never
// left of the video or outside the page.
[[nodiscard]] POINT PanelPosition(const RECT& page, const RECT& video, SIZE panel, int gap);

}  // namespace ldm::ui
