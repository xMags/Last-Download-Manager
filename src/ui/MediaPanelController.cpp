#include "ui/MediaPanelController.h"

#include "base/Utf.h"
#include "core/FileNames.h"
#include "core/Url.h"
#include "ui/Handles.h"
#include "ui/MediaPanelMenuPainter.h"
#include "ui/PanelPlacement.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace ldm::ui {

namespace {

// How often the panel follows the browser window while it shows: window moves, minimizing and
// other windows covering it reach no page script, so the controller looks itself.
constexpr UINT kFollowIntervalMs = 100;
// Between the panel and the video's top edge, in physical pixels.
constexpr int kPanelGap = 4;

// Owner-drawn lines (PanelMenuPainter); each line's item data points at its PanelMenuItem, so
// `items` must outlive the menu.
void AppendItems(HMENU menu, const std::vector<PanelMenuItem>& items, std::vector<PanelAction>& actions)
{
    for (const PanelMenuItem& item : items) {
        MENUITEMINFOW info{sizeof(info)};
        info.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_ID | MIIM_STATE;
        info.fType = MFT_OWNERDRAW | (item.separator ? MFT_SEPARATOR : 0);
        info.dwItemData = reinterpret_cast<ULONG_PTR>(&item);
        if (!item.children.empty()) {
            const HMENU submenu = ::CreatePopupMenu();  // owned by `menu` once inserted
            AppendItems(submenu, item.children, actions);
            info.fMask |= MIIM_SUBMENU;
            info.hSubMenu = submenu;
        } else if (!item.separator && item.action && item.enabled) {
            actions.push_back(*item.action);
            info.wID = static_cast<UINT>(actions.size());  // command ids start at 1
        } else if (!item.separator) {
            info.fState = MFS_GRAYED;
        }
        ::InsertMenuItemW(menu, ::GetMenuItemCount(menu), TRUE, &info);
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
      theme_(theme),
      hooks_(std::move(hooks)),
      catalog_(MediaCatalog::Hooks{
          hooks_.fetch,
          [this](BrowserConnectionId connection, int tabId) { OnCatalogChanged(connection, tabId); },
          hooks_.readXml,
          [] { return std::chrono::system_clock::now(); },
          hooks_.lookUp,
          hooks_.cancelLookup,
      })
{
    (void)panel_.Create(MediaPanelWindow::Callbacks{
                                   [this](POINT anchor) { OpenMenu(anchor); },
                                   [this] {
                                       if (followed_) {
                                           hiddenForPage_.insert(followed_->tab);
                                       }
                                       Place();
                                   },
                                   [this] { Place(); },
                                   [this](HWND menuWindow) { MenuShown(menuWindow); },
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
        const bool sameTab = followed_ && followed_->tab == key;
        if (followed_ && !sameTab) {
            panel_.SetMinWidth(0);  // the width matched another tab's menu
        }
        followed_ = Followed{key, position, sameTab ? followed_->window : nullptr};
        recheckWindow_ = true;
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
            moveWatch_.Watch(nullptr);
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
    const auto view = windows_.Find(host, position, followed_->window, std::exchange(recheckWindow_, false));
    if (!view) {
        hide(true);  // minimized or moving between states: look again on the next tick
        return;
    }
    // Followed from now on as it moves, not only at the next tick.
    followed_->window = view->window;
    moveWatch_.Watch(view->window);
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
    PanelMenuPainter painter(theme_, panel_.Dpi());
    // In full mode the panel and its menu are one width: the wider of the two sets it. The panel
    // grows to the left, so it stays over the video's corner.
    menuLineWidth_ = 0;
    if (!panel_.Mini()) {
        if (menuEdgeDpi_ != panel_.Dpi()) {
            menuEdge_ = -1;
        }
        // Until a menu was seen, a guess; MenuShown corrects it while the menu is open.
        const int edge = menuEdge_ >= 0 ? menuEdge_ : ::GetSystemMetricsForDpi(SM_CXMENUCHECK, panel_.Dpi()) + 6;
        RECT panelRect = panel_.WindowRect();
        const int width = std::max<int>(panelRect.right - panelRect.left, painter.NaturalWidth(items) + edge);
        panel_.SetMinWidth(width);
        menuLineWidth_ = width - edge;
        painter.SetMinimumWidth(menuLineWidth_);
        panelRect = panel_.WindowRect();
        anchor.x = panelRect.left;
    }
    MENUINFO look{sizeof(look)};
    look.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
    look.hbrBack = painter.Background();
    ::SetMenuInfo(menu.get(), &look);
    panel_.SetMenuPainter(&painter);
    // A menu needs its owner in front to close when the user clicks elsewhere. The click on the
    // panel gives LDM the right to come to the front; the browser gets it back when the menu closes.
    menuOpen_ = true;
    ::SetForegroundWindow(panel_.Handle());
    const UINT command = static_cast<UINT>(::TrackPopupMenuEx(menu.get(), TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                                              anchor.x, anchor.y, panel_.Handle(), nullptr));
    ::PostMessageW(panel_.Handle(), WM_NULL, 0, 0);
    panel_.SetMenuPainter(nullptr);
    menuOpen_ = false;
    menuLineWidth_ = 0;
    // Browser events ran while the menu was open; the tab may be gone, so only its key is kept.
    if (command >= 1 && command <= actions.size()) {
        Run(actions[command - 1], key);
    }
    Place();
}

void MediaPanelController::MenuShown(HWND menuWindow)
{
    if (menuLineWidth_ <= 0 || menuWindow == nullptr) {
        return;
    }
    RECT menuRect{};
    ::GetWindowRect(menuWindow, &menuRect);
    const int menuWidth = menuRect.right - menuRect.left;
    menuEdge_ = menuWidth - menuLineWidth_;
    menuEdgeDpi_ = panel_.Dpi();
    // The first menu at this DPI was sized by a guess: the panel follows the menu's real width,
    // and the menu moves under the panel (also when Windows moved it in from the screen edge).
    panel_.SetMinWidth(menuWidth);
    const RECT panelRect = panel_.WindowRect();
    const int panelWidth = panelRect.right - panelRect.left;
    ::SetWindowPos(menuWindow, nullptr, panelRect.left, menuRect.top, std::max(menuWidth, panelWidth),
                   menuRect.bottom - menuRect.top, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
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
