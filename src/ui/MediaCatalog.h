#pragma once

#include "base/Result.h"
#include "browser/Protocol.h"
#include "core/Commands.h"
#include "core/TransferEngine.h"
#include "media/MediaModel.h"
#include "media/QualityList.h"
#include "media/XmlElement.h"
#include "ui/BrowserLink.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ldm::ui {

// One thing a tab can offer in the media panel: a plain media file, or a stream whose manifest
// LDM fetched and read.
struct MediaItem {
    enum class State {
        Fetching,  // the manifest is on its way
        Ready,
        Failed,    // the manifest could not be fetched or read; not offered
    };

    std::int64_t key = 0;  // unique while LDM runs
    browser::MediaKind kind = browser::MediaKind::File;
    std::string url;       // as the browser loaded it
    std::string finalUrl;  // of the manifest, after redirects
    std::optional<std::string> mimeType;
    std::optional<std::int64_t> fileSize;
    // What the browser would send to `url`; the manifest and the media playlist for the length
    // are fetched with them.
    std::vector<model::Cookie> cookies;
    State state = State::Fetching;
    std::optional<media::MediaOffer> offer;  // streams, once ready
    // The manifest came from a loopback or private address, so its pieces may be on one too.
    bool fromPrivateNetwork = false;
    std::optional<double> seconds;           // the stream's length, when known
    media::QualityList qualities;            // what the panel lists for a stream
    std::string failure;                     // why it failed, for the log
    // A video of a supported site (video-sites design): `url` is its page as core::FindSiteVideo
    // builds it, and the offer is the site resolver's. Offered while it is looked up and when that
    // failed too, so the panel can say so; `failure` is then shown.
    bool siteVideo = false;
    std::string siteTitle;      // the video's own title, once known
    std::int64_t lookupId = 0;  // of the LookUpSiteVideoCommand under way or answered
};

// The media of one tab of one connected browser.
struct TabMedia {
    BrowserConnectionId connection = 0;
    int tabId = 0;
    std::optional<std::string> pageUrl;
    std::string pageTitle;
    bool incognito = false;
    std::string userAgent;
    std::vector<MediaItem> items;
    // Set while the tab shows a video of a supported site: its page as core::FindSiteVideo builds
    // it. The tab's items are then that video alone; what its player loads is parts of it.
    std::optional<std::string> sitePage;
};

// What each tab of each browser offers for the media panel (M3 design section 6). UI thread only.
// Manifests are fetched through the core and read here; an answer for a tab whose page changed
// meanwhile is dropped, as is one for an item that is gone.
class MediaCatalog {
public:
    struct Hooks {
        std::function<void(core::FetchManifestCommand)> fetch;
        // A tab's offer changed: what the panel shows for it, or whether it has anything at all.
        std::function<void(BrowserConnectionId, int tabId)> changed;
        // Reads an MPD into its element tree (platform::ReadXml in LDM).
        std::function<base::Result<media::XmlElement>(std::string_view)> readXml;
        std::function<std::chrono::system_clock::time_point()> now;
        // Asks the core what a site video page offers, and calls the question off.
        std::function<void(core::LookUpSiteVideoCommand)> lookUp;
        std::function<void(std::int64_t lookupId)> cancelLookup;
    };

    // A page that reports more than this is not one the panel can present sensibly.
    static constexpr size_t kMaxItemsPerTab = 50;

    explicit MediaCatalog(Hooks hooks);

    void OnMediaFound(BrowserConnectionId connection, browser::MediaFound media);
    void OnReset(BrowserConnectionId connection, int tabId);
    // A tab's page on a supported video site: a new video replaces the tab's offer and is looked
    // up; any other page of the site ends the offer.
    void OnSitePage(BrowserConnectionId connection, browser::SitePage page);
    // The core's answer to a lookup; true when it was one of this catalog's.
    bool OnSiteLookedUp(const core::SiteLookupFinished& finished);
    void OnConnectionClosed(BrowserConnectionId connection);
    // An answer of the core; true when it was one of this catalog's fetches.
    bool OnFetched(const core::FetchFinished& fetched);

    [[nodiscard]] const TabMedia* Find(BrowserConnectionId connection, int tabId) const;
    // The items to list, in the order they were found: ready ones, without the media playlists a
    // listed master playlist already covers. Protected streams only when `includeDrm` (Options:
    // say so when a video is protected).
    [[nodiscard]] std::vector<const MediaItem*> Offered(const TabMedia& tab, bool includeDrm) const;
    // Something to offer, counting a DRM notice only when `includeDrm`.
    [[nodiscard]] bool HasMedia(const TabMedia& tab, bool includeDrm) const;

private:
    enum class FetchPurpose {
        Manifest,  // the item's own manifest
        Length,    // a media playlist of a master playlist, for the stream's length
    };
    struct PendingFetch {
        BrowserConnectionId connection = 0;
        int tabId = 0;
        std::int64_t itemKey = 0;
        FetchPurpose purpose = FetchPurpose::Manifest;
    };
    using TabKey = std::pair<BrowserConnectionId, int>;

    void StartFetch(const TabMedia& tab, const MediaItem& item, const std::string& url, FetchPurpose purpose);
    void ReadManifest(TabMedia& tab, MediaItem& item, const core::FetchFinished& fetched);
    void ReadLength(MediaItem& item, const core::FetchFinished& fetched);
    [[nodiscard]] MediaItem* FindItem(TabMedia& tab, std::int64_t key);
    // Ends the tab's site video, calling its lookup off.
    void DropSiteVideo(TabMedia& tab);
    void CancelLookups(const TabMedia& tab);

    Hooks hooks_;
    std::map<TabKey, TabMedia> tabs_;
    std::map<std::int64_t, PendingFetch> fetches_;  // by fetch id
    std::int64_t lastKey_ = 0;
    std::int64_t lastFetchId_ = 0;
    std::map<std::int64_t, TabKey> lookups_;  // site lookups under way, by id
    std::int64_t lastLookupId_ = 0;
};

}  // namespace ldm::ui
