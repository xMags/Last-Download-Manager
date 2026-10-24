#include "ui/MediaPanelController.h"

#include "base/Utf.h"
#include "core/FileNames.h"
#include "core/Url.h"
#include "ui/Handles.h"
#include "ui/PanelPlacement.h"

#include <algorithm>
#include <vector>

namespace ldm::ui {

namespace {

// How often the panel follows the browser window while it shows: window moves, minimizing and
// other windows covering it reach no page script, so the controller looks itself.
constexpr UINT kFollowIntervalMs = 100;
// Between the panel and the video's top edge, in physical pixels.
constexpr int kPanelGap = 4;

void AppendItems(HMENU menu, const std::vector<PanelMenuItem>& items, std::vector<PanelAction>& actions)
{
    for (const PanelMenuItem& item : items) {
        if (item.separator) {
            ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        } else if (!item.children.empty()) {
            const HMENU submenu = ::CreatePopupMenu();  // owned by `menu` once appended
            AppendItems(submenu, item.children, actions);
            ::AppendMenuW(menu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(submenu), item.text.c_str());
        } else if (item.action && item.enabled) {
            actions.push_back(*item.action);
            ::AppendMenuW(menu, MF_STRING, actions.size(), item.text.c_str());  // command ids start at 1
        } else {
            ::AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, item.text.c_str());
        }
    }
}

// "<page title>.<extension of the address>", or nothing when either is missing.
std::optional<std::string> FileNameFor(const TabMedia& tab, const MediaItem& item)
{
    const auto url = core::ParseUrl(item.url);
    const std::string extension = url ? core::FileExtension(core::FileNameFromUrlPath(url->path)) : std::string();
    std::string title = core::SanitizeFileName(tab.pageTitle);
    if (extension.empty() || title.empty()) {
        return std::nullopt;
    }
    return title + "." + extension;
}

}  // namespace

MediaPanelController::MediaPanelController(BrowserWindows& windows, const Theme& theme, Hooks hooks)
    : windows_(windows),
      hooks_(std::move(hooks)),
      catalog_(MediaCatalog::Hooks{
          hooks_.fetch,
          [this](BrowserConnectionId connection, int tabId) { OnCatalogChanged(connection, tabId); },
          hooks_.readXml,
          [] { return std::chrono::system_clock::now(); },
      })
{
    (void)panel_.Create(theme, MediaPanelWindow::Callbacks{
                                   [this](POINT anchor) { OpenMenu(anchor); },
                                   [this] {
                                       if (followed_) {
                                           hiddenForPage_.insert(followed_->tab);
                                       }
                                       Place();
                                   },
                                   [this] { Place(); },
                               });
}

void MediaPanelController::ApplySettings(const model::Settings& settings)
{
    if (enabled_ == settings.mediaPanelEnabled && drmNotice_ == settings.mediaDrmNotice) {
        return;
    }
    enabled_ = settings.mediaPanelEnabled;
    drmNotice_ = settings.mediaDrmNotice;
    std::vector<TabKey> tabs;
    for (const auto& [key, hasMedia] : told_) {
        tabs.push_back(key);
    }
    for (const TabKey& key : tabs) {
        OnCatalogChanged(key.first, key.second);
    }
    Place();
}

bool MediaPanelController::Offers(const TabMedia& tab) const
{
    return enabled_ && catalog_.HasMedia(tab, drmNotice_);
}

void MediaPanelController::OnCatalogChanged(BrowserConnectionId connection, int tabId)
{
    const TabKey key{connection, tabId};
    const TabMedia* tab = catalog_.Find(connection, tabId);
    const bool hasMedia = tab != nullptr && Offers(*tab);
    const auto told = told_.find(key);
    if (told == told_.end() || told->second != hasMedia) {
        if (hooks_.sendMediaList && (hasMedia || told != told_.end())) {
            hooks_.sendMediaList(connection, tabId, hasMedia);
        }
        told_[key] = hasMedia;
    }
    if (tab == nullptr) {
        told_.erase(key);
    }
    if (followed_ && followed_->tab == key) {
        Place();
    }
}

void MediaPanelController::OnMediaReset(BrowserConnectionId connection, int tabId)
{
    const TabKey key{connection, tabId};
    hiddenForPage_.erase(key);
    if (followed_ && followed_->tab == key) {
        followed_.reset();
    }
    catalog_.OnReset(connection, tabId);
    Place();
}

void MediaPanelController::OnVideoPosition(BrowserConnectionId connection, const browser::VideoPosition& position)
{
    const TabKey key{connection, position.tabId};
    if (position.visible) {
        followed_ = Followed{key, position};
        panel_.StartTimer(kFollowIntervalMs);
    } else if (followed_ && followed_->tab == key) {
        followed_.reset();
    }
    Place();
}

void MediaPanelController::OnConnectionClosed(BrowserConnectionId connection)
{
    catalog_.OnConnectionClosed(connection);
    std::erase_if(told_, [connection](const auto& entry) { return entry.first.first == connection; });
    std::erase_if(hiddenForPage_, [connection](const TabKey& key) { return key.first == connection; });
    if (followed_ && followed_->tab.first == connection) {
        followed_.reset();
    }
    Place();
}

void MediaPanelController::Place()
{
    if (menuOpen_) {
        return;  // the menu belongs to the panel; it stays where it was until the menu closes
    }
    const auto hide = [this](bool keepFollowing) {
        panel_.Hide();
        if (!keepFollowing) {
            panel_.StopTimer();
        }
    };
    if (!followed_) {
        hide(false);
        return;
    }
    const TabMedia* tab = catalog_.Find(followed_->tab.first, followed_->tab.second);
    if (tab == nullptr || !Offers(*tab) || hiddenForPage_.contains(followed_->tab)) {
        hide(false);
        return;
    }
    const std::uint32_t host = hooks_.hostProcessOf ? hooks_.hostProcessOf(followed_->tab.first) : 0;
    const browser::VideoPosition& position = followed_->position;
    const auto view = windows_.Find(host, position);
    if (!view) {
        hide(true);  // minimized or moving between states: look again on the next tick
        return;
    }
    const SIZE size = panel_.SizeAt(POINT{view->page.left, view->page.top});
    const auto video = VideoOnScreen(view->page, position.video, position.pixelRatio, SIZE{size.cx, size.cy * 2});
    if (!video) {
        hide(true);
        return;
    }
    const POINT center{(video->left + video->right) / 2, (video->top + video->bottom) / 2};
    const POINT at = PanelPosition(view->page, *video, size, kPanelGap);
    const POINT panelMiddle{at.x + size.cx / 2, at.y + size.cy / 2};
    if (!windows_.IsShownAt(*view, center, panel_.Handle()) || !windows_.IsShownAt(*view, panelMiddle, panel_.Handle())) {
        hide(true);
        return;
    }
    const auto offered = catalog_.Offered(*tab, drmNotice_);
    const bool onlyProtected = std::all_of(offered.begin(), offered.end(),
                                           [](const MediaItem* item) { return item->qualities.drm.has_value(); });
    panel_.SetContent(onlyProtected);
    panel_.ShowAt(at);
}

void MediaPanelController::OpenMenu(POINT anchor)
{
    if (!followed_) {
        return;
    }
    const TabKey key = followed_->tab;
    const TabMedia* tab = catalog_.Find(key.first, key.second);
    if (tab == nullptr) {
        return;
    }
    const std::vector<PanelMenuItem> items = BuildPanelMenu(catalog_.Offered(*tab, drmNotice_), panel_.Mini());
    UniqueMenu menu(::CreatePopupMenu());
    std::vector<PanelAction> actions;
    AppendItems(menu.get(), items, actions);
    // A menu needs its owner in front to close when the user clicks elsewhere. The click on the
    // panel gives LDM the right to come to the front; the browser gets it back when the menu closes.
    menuOpen_ = true;
    ::SetForegroundWindow(panel_.Handle());
    const UINT command = static_cast<UINT>(::TrackPopupMenuEx(menu.get(), TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                                              anchor.x, anchor.y, panel_.Handle(), nullptr));
    ::PostMessageW(panel_.Handle(), WM_NULL, 0, 0);
    menuOpen_ = false;
    // Browser events ran while the menu was open; the tab may be gone, so only its key is kept.
    if (command >= 1 && command <= actions.size()) {
        Run(actions[command - 1], key);
    }
    Place();
}

void MediaPanelController::Run(const PanelAction& action, const TabKey& key)
{
    switch (action.kind) {
    case PanelAction::Kind::SmallPanel:
        panel_.SetMini(true);
        return;
    case PanelAction::Kind::FullPanel:
        panel_.SetMini(false);
        return;
    case PanelAction::Kind::HideForPage:
        hiddenForPage_.insert(key);
        return;
    case PanelAction::Kind::DownloadStream:
    case PanelAction::Kind::DownloadSubtitles:
    case PanelAction::Kind::DownloadFile:
        break;
    }
    const TabMedia* tab = catalog_.Find(key.first, key.second);
    if (tab == nullptr) {
        return;
    }
    const auto item = std::find_if(tab->items.begin(), tab->items.end(),
                                   [&action](const MediaItem& candidate) { return candidate.key == action.itemKey; });
    if (item == tab->items.end()) {
        return;
    }
    if (action.kind != PanelAction::Kind::DownloadFile) {
        auto capture = BuildStreamCapture(*tab, *item, action);
        if (!capture) {
            if (hooks_.tell) {
                hooks_.tell(base::ToUtf16(capture.GetError().message));
            }
            return;
        }
        if (hooks_.downloadStream) {
            hooks_.downloadStream(std::move(capture).Value());
        }
        return;
    }
    if (!hooks_.downloadFile) {
        return;
    }
    browser::Capture capture;
    capture.url = item->url;
    capture.referrer = tab->pageUrl;
    capture.fileName = FileNameFor(*tab, *item);
    capture.fileSize = item->fileSize;
    capture.mimeType = item->mimeType;
    capture.userAgent = tab->userAgent;
    capture.cookies = item->cookies;
    capture.chosen = true;  // the user picked it; a cancel says nothing about the capture rules
    hooks_.downloadFile(std::move(capture));
}

}  // namespace ldm::ui
