#pragma once

#include "core/Commands.h"
#include "core/Ports.h"
#include "core/SiteResolver.h"
#include "core/Snapshot.h"
#include "core/TransferEngine.h"
#include "model/Ids.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ldm::core {

// Lookups of site video pages (video-sites design, sections 3 and 8), on the core thread beside
// the coordinator. Each video is asked about at most once at a time: the media panel's lookups
// and the refreshes of downloads whose addresses expired wait for the same answer. Answers are
// kept for a while, so reopening a video costs nothing. Each lookup picks its proxy exactly as a
// download does.
class SiteLookups {
public:
    // A refresh of download `id` (run `epoch`) was answered.
    using Refreshed = std::function<void(model::DownloadId id, std::int64_t epoch, const SiteLookupFinished&)>;

    SiteLookups(SiteResolver& resolver, ProxyResolver& proxy, CoreObserver& observer, Clock& clock, Refreshed refreshed);

    // The panel's lookup: answered with a SiteLookupFinished notification under its lookupId.
    void LookUp(const LookUpSiteVideoCommand& command);
    void Cancel(std::int64_t panelLookupId);
    // A download needs fresh addresses for `pageUrl`: answered through `refreshed`. An answer is
    // reused only when it is very recent, since the old addresses are what failed.
    void Refresh(model::DownloadId id, std::int64_t epoch, const std::string& pageUrl);
    void CancelRefresh(model::DownloadId id);

    // Events with an id for which IsSiteLookupKey holds.
    void OnProxyResolved(model::DownloadId key, const std::optional<std::string>& proxy);
    void OnFinished(model::DownloadId key, const SiteLookupFinished& finished);

    // Answers are kept this long for the panel, and this long for refreshes.
    static constexpr std::chrono::minutes kPanelReuse{30};
    static constexpr std::chrono::seconds kRefreshReuse{60};
    // Videos asked about at once, beyond which a lookup is refused; and answers kept.
    static constexpr size_t kMaxPending = 16;
    static constexpr size_t kMaxCached = 64;

private:
    using RefreshKey = std::pair<model::DownloadId, std::int64_t>;  // download and run

    struct Pending {
        std::string pageUrl;
        bool awaitingProxy = false;
        std::vector<std::int64_t> panelIds;
        std::vector<RefreshKey> refreshes;
    };
    struct Cached {
        media::SiteVideo video;
        std::chrono::steady_clock::time_point at;
    };

    // The lookup for `pageUrl` that is under way, or a new one; nothing when too many are.
    [[nodiscard]] Pending* Join(const std::string& pageUrl);
    void Start(std::int64_t lookupId, Pending& pending);
    [[nodiscard]] const Cached* Recent(const std::string& pageUrl, std::chrono::steady_clock::duration age);
    void Remember(const std::string& pageUrl, const media::SiteVideo& video);
    void AnswerPanel(std::int64_t panelId, SiteLookupFinished answer);
    // Drops the lookup when nobody waits for its answer any more.
    void DropIfUnwanted(std::map<std::int64_t, Pending>::iterator it);

    SiteResolver& resolver_;
    ProxyResolver& proxy_;
    CoreObserver& observer_;
    Clock& clock_;
    Refreshed refreshed_;
    std::map<std::int64_t, Pending> pending_;  // by the resolver's lookup id
    std::map<std::string, Cached> cache_;      // by page
    std::int64_t lastLookupId_ = 0;
};

}  // namespace ldm::core
