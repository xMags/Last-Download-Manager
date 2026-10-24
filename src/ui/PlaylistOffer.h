#pragma once

#include "core/Snapshot.h"
#include "media/M3uList.h"
#include "ui/Theme.h"

#include <windows.h>

#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ldm::ui {

// A file name LDM reads as a playlist of links when it finishes: ".m3u" or ".m3u8".
[[nodiscard]] bool IsPlaylistFileName(std::string_view name);

// Offers the links of a downloaded playlist file (checklist A-12): when an M3U file that lists
// links completes, LDM asks whether to add them, then shows them in the Download All Links dialog.
// HLS playlists are left alone. UI thread only.
class PlaylistOffer {
public:
    struct Context {
        HWND owner = nullptr;
        const Theme* theme = nullptr;
        // Shows the links for picking and adds the checked ones (AddFlow::AddPlaylistLinks).
        std::function<void(const std::vector<media::M3uEntry>&, const std::string&)> addLinks;
    };

    explicit PlaylistOffer(Context context);

    void OnCompleted(const core::DownloadCompleted& completed);
    // Makes the offers that waited for a dialog to close. Call when a snapshot arrives.
    void OfferWaiting();

private:
    struct Offer {
        std::string fileName;
        std::string url;
        std::vector<media::M3uEntry> entries;
    };

    Context context_;
    // A completion does not come again, so an offer that arrives while a dialog is open waits
    // here instead of being dropped.
    std::deque<Offer> waiting_;
    bool offering_ = false;  // a question is open; its modal loop must not start another
};

}  // namespace ldm::ui
