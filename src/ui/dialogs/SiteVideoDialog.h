#pragma once

#include "browser/Protocol.h"
#include "core/Commands.h"
#include "core/TransferEngine.h"
#include "ui/Dialog.h"
#include "ui/MediaCatalog.h"
#include "ui/MediaPanelMenu.h"
#include "ui/StreamChoice.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace ldm::ui {

// "Download video" for a page the user named (video-sites round 2, section 2.2): the right-click
// menu's item for a page or a link, or a page address in Add URL. Asks the site resolver about the
// page, then lists what it offers as the media panel would, and gives the chosen quality as the
// stream the panel would give. Modal; the resolver's answer arrives through OnSiteLookedUp while
// it is open.
class SiteVideoDialog final : public Dialog {
public:
    struct Hooks {
        std::function<void(core::LookUpSiteVideoCommand)> lookUp;
        std::function<void(std::int64_t)> cancelLookup;
    };

    SiteVideoDialog(const Theme& theme, browser::SiteRequest request, std::int64_t lookupId, Hooks hooks);

    // The chosen stream; nothing when the user cancelled, the page offered nothing, or it lists
    // several videos (TakePlaylist).
    [[nodiscard]] std::optional<StreamCapture> Run(HWND owner);
    // The videos of the page, when it lists several: the dialog closed for them to be picked.
    [[nodiscard]] std::optional<media::SitePlaylist> TakePlaylist() { return std::move(playlist_); }
    // An answer of the resolver; false when it is not for this dialog.
    bool OnSiteLookedUp(const core::SiteLookupFinished& finished);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;
    void ShowOffer();
    void Choose();

    browser::SiteRequest request_;
    std::int64_t lookupId_ = 0;
    Hooks hooks_;
    bool pending_ = false;
    TabMedia tab_;
    MediaItem item_;
    std::vector<PanelAction> actions_;  // per line of the list
    std::optional<StreamCapture> result_;
    std::optional<media::SitePlaylist> playlist_;
};

}  // namespace ldm::ui
