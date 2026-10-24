#pragma once

#include "ui/MediaCatalog.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ldm::ui {

// What a click in the media panel's menu does.
struct PanelAction {
    enum class Kind {
        DownloadFile,       // a plain media file: the ordinary add flow
        DownloadStream,     // `entry` of the item's quality list
        DownloadSubtitles,  // `entry` of the item's subtitle list
        SmallPanel,
        FullPanel,
        HideForPage,
    };
    Kind kind = Kind::DownloadFile;
    std::int64_t itemKey = 0;
    size_t entry = 0;
};

// One line of the menu: a command, a disabled notice, a separator or a submenu.
struct PanelMenuItem {
    std::wstring text;  // a tab separates the size, which menus show on the right
    bool enabled = true;
    bool separator = false;
    std::optional<PanelAction> action;
    std::vector<PanelMenuItem> children;  // a submenu when not empty
};

// The menu for a tab's offered media (checklist B-07): with one stream, its qualities at the top
// and "Video only", "Audio only" and "Subtitles" submenus; with several, one submenu per stream;
// plain files as single lines; a protected stream as a disabled notice. Then the panel's own
// commands.
[[nodiscard]] std::vector<PanelMenuItem> BuildPanelMenu(const std::vector<const MediaItem*>& offered, bool mini);

}  // namespace ldm::ui
