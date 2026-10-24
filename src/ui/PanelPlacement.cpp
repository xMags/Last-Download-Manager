#include "ui/PanelPlacement.h"

#include <algorithm>
#include <cmath>

namespace ldm::ui {

std::optional<RECT> VideoOnScreen(const RECT& page, const browser::CssRect& video, double pixelRatio, SIZE minimum)
{
    const auto at = [pixelRatio](LONG origin, double css) {
        return static_cast<LONG>(std::lround(static_cast<double>(origin) + css * pixelRatio));
    };
    const RECT full{at(page.left, video.x), at(page.top, video.y), at(page.left, video.x + video.width),
                    at(page.top, video.y + video.height)};
    RECT shown{};
    if (!::IntersectRect(&shown, &full, &page)) {
        return std::nullopt;
    }
    if (shown.right - shown.left < minimum.cx || shown.bottom - shown.top < minimum.cy) {
        return std::nullopt;
    }
    return shown;
}

POINT PanelPosition(const RECT& page, const RECT& video, SIZE panel, int gap)
{
    LONG x = video.right - panel.cx;
    x = std::max({x, video.left, page.left});
    x = std::min(x, page.right - panel.cx);
    LONG y = video.top - panel.cy - gap;
    if (y < page.top) {
        y = video.top + gap;
    }
    y = std::min(y, page.bottom - panel.cy);
    return POINT{x, y};
}

}  // namespace ldm::ui
