#pragma once

#include "media/SiteVideo.h"
#include "model/Media.h"
#include "ui/Dialog.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace ldm::ui {

// The videos of a playlist, a channel or a page with several (video-sites round 2, section 2.4):
// check the ones to add, pick how each picks its quality when it starts, and whether they start
// at once or wait in a queue.
class SitePlaylistDialog final : public Dialog {
public:
    struct Choice {
        std::vector<size_t> entries;  // checked, in the list's order
        model::SiteQuality quality = model::SiteQuality::Best;
        bool start = false;
    };

    SitePlaylistDialog(const Theme& theme, const media::SitePlaylist& playlist);

    // Nothing when cancelled or nothing was checked.
    [[nodiscard]] std::optional<Choice> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;
    bool OnNotify(const NMHDR& header, LRESULT& result) override;
    void CheckAll(bool checked);
    void UpdateCount();

    const media::SitePlaylist& playlist_;
    std::optional<Choice> result_;
    bool filling_ = false;  // check boxes set by the dialog itself: counted once at the end
};

}  // namespace ldm::ui
