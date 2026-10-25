#include "core/SiteLookups.h"

#include "core/SiteList.h"

#include <algorithm>
#include <iterator>

namespace ldm::core {

namespace {

SiteLookupFinished Failed(SiteLookupFinished::Problem problem, std::string message)
{
    SiteLookupFinished finished;
    finished.problem = problem;
    finished.failure = std::move(message);
    return finished;
}

}  // namespace

SiteLookups::SiteLookups(SiteResolver& resolver, ProxyResolver& proxy, CoreObserver& observer, Clock& clock,
                         Refreshed refreshed)
    : resolver_(resolver), proxy_(proxy), observer_(observer), clock_(clock), refreshed_(std::move(refreshed))
{
}

void SiteLookups::LookUp(const LookUpSiteVideoCommand& command)
{
    if (command.lookupId <= 0) {
        return;  // no id to answer under
    }
    const auto page = CheckSitePage(command.pageUrl);
    if (!page) {
        AnswerPanel(command.lookupId,
                    Failed(SiteLookupFinished::Problem::Video, "This page is not one LDM can look for videos on."));
        return;
    }
    if (const Cached* cached = Recent(page->address, kPanelReuse)) {
        SiteLookupFinished answer;
        answer.video = cached->video;
        AnswerPanel(command.lookupId, std::move(answer));
        return;
    }
    Pending* pending = Join(page->address);
    if (pending == nullptr) {
        AnswerPanel(command.lookupId, Failed(SiteLookupFinished::Problem::Busy, "Too many videos are being looked up at once."));
        return;
    }
    pending->panelIds.push_back(command.lookupId);
}

void SiteLookups::Cancel(std::int64_t panelLookupId)
{
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        auto& ids = it->second.panelIds;
        if (const auto found = std::find(ids.begin(), ids.end(), panelLookupId); found != ids.end()) {
            ids.erase(found);
            DropIfUnwanted(it);
            return;
        }
    }
}

void SiteLookups::Refresh(model::DownloadId id, std::int64_t epoch, const std::string& pageUrl)
{
    const auto page = CheckSitePage(pageUrl);
    if (!page) {
        refreshed_(id, epoch, Failed(SiteLookupFinished::Problem::Video, "The download has no video page to ask again."));
        return;
    }
    if (const Cached* cached = Recent(page->address, kRefreshReuse)) {
        SiteLookupFinished answer;
        answer.video = cached->video;
        refreshed_(id, epoch, answer);
        return;
    }
    Pending* pending = Join(page->address);
    if (pending == nullptr) {
        refreshed_(id, epoch, Failed(SiteLookupFinished::Problem::Busy, "Too many videos are being looked up at once."));
        return;
    }
    pending->refreshes.emplace_back(id, epoch);
}

void SiteLookups::CancelRefresh(model::DownloadId id)
{
    for (auto it = pending_.begin(); it != pending_.end();) {
        auto& refreshes = it->second.refreshes;
        std::erase_if(refreshes, [id](const RefreshKey& key) { return key.first == id; });
        auto next = std::next(it);
        DropIfUnwanted(it);
        it = next;
    }
}

void SiteLookups::OnProxyResolved(model::DownloadId key, const std::optional<std::string>& proxy)
{
    const std::int64_t lookupId = SiteLookupIdOf(key);
    const auto it = pending_.find(lookupId);
    if (it == pending_.end() || !it->second.awaitingProxy) {
        return;  // dropped meanwhile
    }
    it->second.awaitingProxy = false;
    resolver_.Resolve(SiteLookupRequest{lookupId, it->second.pageUrl, proxy});
}

void SiteLookups::OnFinished(model::DownloadId key, const SiteLookupFinished& finished)
{
    const auto it = pending_.find(SiteLookupIdOf(key));
    if (it == pending_.end()) {
        return;  // dropped: nobody waits for it
    }
    // Taken out first: the answers below may start new lookups, also for the same page.
    const Pending pending = std::move(it->second);
    pending_.erase(it);
    SiteLookupFinished answer = finished;
    if (answer.stopped) {
        // Only LDM's own shutdown stops a lookup somebody still waits for.
        answer = Failed(SiteLookupFinished::Problem::Busy, "The lookup was stopped.");
    }
    if (answer.video) {
        Remember(pending.pageUrl, *answer.video);
    }
    for (const std::int64_t panelId : pending.panelIds) {
        AnswerPanel(panelId, answer);
    }
    for (const auto& [id, epoch] : pending.refreshes) {
        refreshed_(id, epoch, answer);
    }
}

SiteLookups::Pending* SiteLookups::Join(const std::string& pageUrl)
{
    for (auto& [id, pending] : pending_) {
        if (pending.pageUrl == pageUrl) {
            return &pending;
        }
    }
    if (pending_.size() >= kMaxPending || lastLookupId_ >= kMaxSiteLookupId) {
        return nullptr;
    }
    const std::int64_t lookupId = ++lastLookupId_;
    Pending& pending = pending_[lookupId];
    pending.pageUrl = pageUrl;
    Start(lookupId, pending);
    return &pending;
}

void SiteLookups::Start(std::int64_t lookupId, Pending& pending)
{
    const ProxyLookup lookup = proxy_.Resolve(SiteLookupKey(lookupId), 0, pending.pageUrl);
    if (lookup.pending) {
        pending.awaitingProxy = true;
        return;
    }
    resolver_.Resolve(SiteLookupRequest{lookupId, pending.pageUrl, lookup.proxy});
}

const SiteLookups::Cached* SiteLookups::Recent(const std::string& pageUrl, std::chrono::steady_clock::duration age)
{
    const auto it = cache_.find(pageUrl);
    if (it == cache_.end()) {
        return nullptr;
    }
    const auto now = clock_.Steady();
    if (now - it->second.at > kPanelReuse) {
        cache_.erase(it);  // too old for anyone
        return nullptr;
    }
    return now - it->second.at <= age ? &it->second : nullptr;
}

void SiteLookups::Remember(const std::string& pageUrl, const media::SiteVideo& video)
{
    cache_[pageUrl] = Cached{video, clock_.Steady()};
    while (cache_.size() > kMaxCached) {
        const auto oldest = std::min_element(cache_.begin(), cache_.end(),
                                             [](const auto& a, const auto& b) { return a.second.at < b.second.at; });
        cache_.erase(oldest);
    }
}

void SiteLookups::AnswerPanel(std::int64_t panelId, SiteLookupFinished answer)
{
    answer.lookupId = panelId;
    observer_.OnNotification(std::move(answer));
}

void SiteLookups::DropIfUnwanted(std::map<std::int64_t, Pending>::iterator it)
{
    if (!it->second.panelIds.empty() || !it->second.refreshes.empty()) {
        return;
    }
    const bool asked = !it->second.awaitingProxy;
    const std::int64_t lookupId = it->first;
    pending_.erase(it);
    if (asked) {
        resolver_.Cancel(lookupId);  // its stopped answer finds nothing waiting
    }
}

}  // namespace ldm::core
