#include "ui/MediaCatalog.h"

#include "media/DashManifest.h"
#include "media/HlsPlaylist.h"

#include <algorithm>
#include <set>
#include <variant>

namespace ldm::ui {

namespace {

// The addresses of the media playlists a master playlist names: when the browser loads them too,
// they are parts of that stream, not streams of their own.
void AddPlaylistAddresses(const media::MediaOffer& offer, std::set<std::string>& into)
{
    const auto add = [&into](const media::SegmentSource& source) {
        if (const auto* address = std::get_if<media::PlaylistAddress>(&source)) {
            into.insert(address->url);
        }
    };
    for (const media::Variant& variant : offer.variants) {
        add(variant.source);
    }
    for (const auto* renditions : {&offer.audio, &offer.subtitles}) {
        for (const media::Rendition& rendition : *renditions) {
            if (rendition.source) {
                add(*rendition.source);
            }
        }
    }
}

// The first media playlist of a master playlist, whose length is the stream's.
std::optional<std::string> FirstPlaylist(const media::MediaOffer& offer)
{
    for (const media::Variant& variant : offer.variants) {
        if (const auto* address = std::get_if<media::PlaylistAddress>(&variant.source)) {
            return address->url;
        }
    }
    return std::nullopt;
}

}  // namespace

MediaCatalog::MediaCatalog(Hooks hooks) : hooks_(std::move(hooks)) {}

void MediaCatalog::OnMediaFound(BrowserConnectionId connection, browser::MediaFound media)
{
    TabMedia& tab = tabs_[TabKey{connection, media.tabId}];
    tab.connection = connection;
    tab.tabId = media.tabId;
    tab.pageUrl = media.pageUrl;
    tab.pageTitle = media.pageTitle;
    tab.incognito = media.incognito;
    tab.userAgent = media.userAgent;
    const bool known = std::any_of(tab.items.begin(), tab.items.end(),
                                   [&media](const MediaItem& item) { return item.url == media.url; });
    if (known || tab.items.size() >= kMaxItemsPerTab) {
        return;
    }
    MediaItem item;
    item.key = ++lastKey_;
    item.kind = media.kind;
    item.url = media.url;
    item.finalUrl = media.url;
    item.mimeType = media.mimeType;
    item.fileSize = media.fileSize;
    item.cookies = std::move(media.cookies);
    item.state = media.kind == browser::MediaKind::File ? MediaItem::State::Ready : MediaItem::State::Fetching;
    tab.items.push_back(std::move(item));
    const MediaItem& added = tab.items.back();
    if (added.kind != browser::MediaKind::File) {
        StartFetch(tab, added, added.url, FetchPurpose::Manifest);
    }
    if (hooks_.changed) {
        hooks_.changed(connection, media.tabId);
    }
}

void MediaCatalog::OnReset(BrowserConnectionId connection, int tabId)
{
    if (tabs_.erase(TabKey{connection, tabId}) > 0 && hooks_.changed) {
        hooks_.changed(connection, tabId);
    }
    // Fetches of the tab still under way are answered later; their answers find no tab.
}

void MediaCatalog::OnConnectionClosed(BrowserConnectionId connection)
{
    for (auto it = tabs_.begin(); it != tabs_.end();) {
        it = it->first.first == connection ? tabs_.erase(it) : std::next(it);
    }
}

bool MediaCatalog::OnFetched(const core::FetchFinished& fetched)
{
    const auto pending = fetches_.find(fetched.fetchId);
    if (pending == fetches_.end()) {
        return false;
    }
    const PendingFetch fetch = pending->second;
    fetches_.erase(pending);
    const auto tab = tabs_.find(TabKey{fetch.connection, fetch.tabId});
    if (tab == tabs_.end()) {
        return true;  // the page changed meanwhile
    }
    MediaItem* item = FindItem(tab->second, fetch.itemKey);
    if (item == nullptr) {
        return true;
    }
    if (fetch.purpose == FetchPurpose::Manifest) {
        ReadManifest(tab->second, *item, fetched);
    } else {
        ReadLength(*item, fetched);
    }
    if (hooks_.changed) {
        hooks_.changed(fetch.connection, fetch.tabId);
    }
    return true;
}

const TabMedia* MediaCatalog::Find(BrowserConnectionId connection, int tabId) const
{
    const auto it = tabs_.find(TabKey{connection, tabId});
    return it == tabs_.end() ? nullptr : &it->second;
}

std::vector<const MediaItem*> MediaCatalog::Offered(const TabMedia& tab, bool includeDrm) const
{
    std::set<std::string> covered;
    for (const MediaItem& item : tab.items) {
        if (item.state == MediaItem::State::Ready && item.offer) {
            AddPlaylistAddresses(*item.offer, covered);
        }
    }
    std::vector<const MediaItem*> offered;
    for (const MediaItem& item : tab.items) {
        if (item.state != MediaItem::State::Ready) {
            continue;
        }
        if (item.kind == browser::MediaKind::Hls && (covered.contains(item.url) || covered.contains(item.finalUrl))) {
            continue;
        }
        if (item.kind != browser::MediaKind::File && item.qualities.entries.empty() && !item.qualities.drm) {
            continue;  // a stream with nothing to choose
        }
        if (item.qualities.drm && !includeDrm) {
            continue;
        }
        offered.push_back(&item);
    }
    return offered;
}

bool MediaCatalog::HasMedia(const TabMedia& tab, bool includeDrm) const
{
    return !Offered(tab, includeDrm).empty();
}

void MediaCatalog::StartFetch(const TabMedia& tab, const MediaItem& item, const std::string& url, FetchPurpose purpose)
{
    if (!hooks_.fetch) {
        return;
    }
    const std::int64_t id = ++lastFetchId_;
    fetches_[id] = PendingFetch{tab.connection, tab.tabId, item.key, purpose};
    core::FetchManifestCommand command;
    command.fetchId = id;
    command.url = url;
    command.referer = tab.pageUrl;
    command.userAgent = tab.userAgent;
    command.cookies = item.cookies;
    command.maxBytes = media::kMaxManifestBytes;
    hooks_.fetch(std::move(command));
}

void MediaCatalog::ReadManifest(TabMedia& tab, MediaItem& item, const core::FetchFinished& fetched)
{
    if (fetched.failure) {
        item.state = MediaItem::State::Failed;
        item.failure = fetched.failure->message;
        return;
    }
    item.finalUrl = fetched.finalUrl.empty() ? item.url : fetched.finalUrl;
    item.fromPrivateNetwork = fetched.fromPrivateNetwork;
    if (item.kind == browser::MediaKind::Dash) {
        auto root = hooks_.readXml ? hooks_.readXml(fetched.body)
                                   : base::Result<media::XmlElement>(base::MakeError(base::ErrorCode::Unsupported,
                                                                                      "no XML reader"));
        auto offer = root ? media::ParseMpd(root.Value(), item.finalUrl,
                                            hooks_.now ? hooks_.now() : std::chrono::system_clock::now())
                          : base::Result<media::MediaOffer>(root.GetError());
        if (!offer) {
            item.state = MediaItem::State::Failed;
            item.failure = offer.GetError().message;
            return;
        }
        item.offer = std::move(offer).Value();
        item.seconds = item.offer->seconds;
    } else {
        auto playlist = media::ParseHls(fetched.body, item.finalUrl);
        if (!playlist) {
            item.state = MediaItem::State::Failed;
            item.failure = playlist.GetError().message;
            return;
        }
        if (auto* offer = std::get_if<media::MediaOffer>(&playlist.Value())) {
            item.offer = std::move(*offer);
            if (const auto first = FirstPlaylist(*item.offer)) {
                StartFetch(tab, item, *first, FetchPurpose::Length);
            }
        } else {
            // A media playlist on its own: one quality, already described in full.
            auto& track = std::get<media::TrackSegments>(playlist.Value());
            media::MediaOffer single;
            single.format = media::StreamFormat::Hls;
            single.live = track.live;
            single.drm = track.drm;
            if (!track.live) {
                item.seconds = media::TotalSeconds(track);
            }
            media::Variant variant;
            variant.source = std::move(track);
            single.variants.push_back(std::move(variant));
            item.offer = std::move(single);
        }
    }
    item.qualities = media::BuildQualityList(*item.offer, item.seconds);
    item.state = MediaItem::State::Ready;
}

void MediaCatalog::ReadLength(MediaItem& item, const core::FetchFinished& fetched)
{
    if (fetched.failure || !item.offer) {
        return;  // the qualities stay, without sizes
    }
    auto playlist = media::ParseHls(fetched.body, fetched.finalUrl.empty() ? item.finalUrl : fetched.finalUrl);
    if (!playlist) {
        return;
    }
    const auto* track = std::get_if<media::TrackSegments>(&playlist.Value());
    if (track == nullptr) {
        return;
    }
    if (track->drm) {
        // A master playlist need not announce its keys; its media playlists show them.
        item.offer->drm = track->drm;
    }
    item.offer->live = track->live;
    if (!track->live) {
        item.seconds = media::TotalSeconds(*track);
    }
    item.qualities = media::BuildQualityList(*item.offer, item.seconds);
}

MediaItem* MediaCatalog::FindItem(TabMedia& tab, std::int64_t key)
{
    const auto it = std::find_if(tab.items.begin(), tab.items.end(), [key](const MediaItem& item) { return item.key == key; });
    return it == tab.items.end() ? nullptr : &*it;
}

}  // namespace ldm::ui
