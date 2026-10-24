#pragma once

#include "base/Result.h"
#include "core/Commands.h"
#include "model/Cookie.h"
#include "ui/MediaCatalog.h"
#include "ui/MediaPanelMenu.h"

#include <optional>
#include <string>
#include <vector>

namespace ldm::ui {

// A stream the user picked in the media panel, ready for the add flow: the manifest's address,
// the tracks of the chosen quality and what the browser would send with them.
struct StreamCapture {
    std::string manifestUrl;
    std::string fileName;  // "<page title>.mp4", ".m4a" for sound only, "<title>.<language>.vtt"
    std::optional<std::string> pageUrl;
    std::string userAgent;
    std::vector<model::Cookie> cookies;
    core::MediaChoice media;
};

// The capture for a DownloadStream or DownloadSubtitles action on `item` of `tab`. Fails, with
// the words to show, for a live stream and for an entry that no longer exists.
[[nodiscard]] base::Result<StreamCapture> BuildStreamCapture(const TabMedia& tab, const MediaItem& item,
                                                             const PanelAction& action);

}  // namespace ldm::ui
