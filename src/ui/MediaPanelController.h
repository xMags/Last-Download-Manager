#pragma once

#include "browser/Protocol.h"
#include "core/Commands.h"
#include "core/TransferEngine.h"
#include "model/Settings.h"
#include "ui/BrowserLink.h"
#include "ui/BrowserWindows.h"
#include "ui/MediaCatalog.h"
#include "ui/MediaPanelMenu.h"
#include "ui/MediaPanelWindow.h"
#include "ui/StreamChoice.h"
#include "ui/Theme.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace ldm::ui {

// The media panel over web players (checklist B-07, B-09; M3 design section 6): keeps what each
// tab offers (MediaCatalog), tells each extension which tabs to follow, places the panel over the
// video of the tab the user looks at, and turns menu choices into downloads. UI thread only.
class MediaPanelController {
public:
    struct Hooks {
        std::function<void(core::FetchManifestCommand)> fetch;
        std::function<void(BrowserConnectionId, int tabId, bool hasMedia)> sendMediaList;
        std::function<std::uint32_t(BrowserConnectionId)> hostProcessOf;
        std::function<void(browser::Capture)> downloadFile;  // the ordinary add flow
        std::function<void(StreamCapture)> downloadStream;   // the add flow, for a stream
        std::function<void(const std::wstring&)> tell;       // a message to the user
        std::function<base::Result<media::XmlElement>(std::string_view)> readXml;
        std::function<void(core::LookUpSiteVideoCommand)> lookUp;
        std::function<void(std::int64_t lookupId)> cancelLookup;
    };

    MediaPanelController(BrowserWindows& windows, const Theme& theme, Hooks hooks);
    MediaPanelController(const MediaPanelController&) = delete;
    MediaPanelController& operator=(const MediaPanelController&) = delete;

    void ApplySettings(const model::Settings& settings);
    void ThemeChanged() { panel_.ThemeChanged(); }

    void OnMedia(BrowserConnectionId connection, browser::MediaFound media) { catalog_.OnMediaFound(connection, std::move(media)); }
    void OnMediaReset(BrowserConnectionId connection, int tabId);
    void OnSitePage(BrowserConnectionId connection, browser::SitePage page) { catalog_.OnSitePage(connection, std::move(page)); }
    // A core answer; true when it was one of the panel's site lookups.
    bool OnSiteLookedUp(const core::SiteLookupFinished& finished) { return catalog_.OnSiteLookedUp(finished); }
    void OnVideoPosition(BrowserConnectionId connection, const browser::VideoPosition& position);
    void OnConnectionClosed(BrowserConnectionId connection);
    // A core answer; true when it was one of the panel's manifest fetches.
    bool OnFetched(const core::FetchFinished& fetched) { return catalog_.OnFetched(fetched); }

private:
    using TabKey = std::pair<BrowserConnectionId, int>;

    // The tab whose video the panel follows: the last one reported visible.
    struct Followed {
        TabKey tab;
        browser::VideoPosition position;
    };

    void OnCatalogChanged(BrowserConnectionId connection, int tabId);
    void Place();
    void OpenMenu(POINT anchor);
    void Run(const PanelAction& action, const TabKey& key);
    [[nodiscard]] bool Offers(const TabMedia& tab) const;

    BrowserWindows& windows_;
    Hooks hooks_;
    MediaCatalog catalog_;
    MediaPanelWindow panel_;
    bool enabled_ = true;
    bool drmNotice_ = true;
    std::optional<Followed> followed_;
    std::map<TabKey, bool> told_;     // what each extension was last told per tab
    std::set<TabKey> hiddenForPage_;  // closed by the user until the tab loads another page
    bool menuOpen_ = false;
};

}  // namespace ldm::ui
