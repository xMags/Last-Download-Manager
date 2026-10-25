#include "ui/MainWindow.h"

#include "base/Utf.h"
#include "resource.h"
#include "ui/Dpi.h"
#include "ui/Format.h"
#include "ui/dialogs/MessageDialog.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace ldm::ui {

namespace {

constexpr wchar_t kClassName[] = L"LdmMainWindow";
constexpr int kToolbarId = 100;
constexpr int kSplitterWidth96 = 5;
constexpr int kMargin96 = 6;   // grey border around the panes
constexpr int kTopGap96 = 12;  // space between the toolbar line and the panes
constexpr int kDividerPx = 1;  // hairlines stay one physical pixel at every DPI
constexpr int kMinimumTreeWidth96 = 120;
constexpr int kMinimumListWidth96 = 200;
constexpr UINT kSaveLayoutMessage = WM_APP + 10;
constexpr UINT kExternalInputMessage = WM_APP + 11;
constexpr UINT kBrowserEventMessage = WM_APP + 12;
constexpr UINT_PTR kExternalInputRetryTimer = 1;
constexpr UINT kExternalInputRetryMs = 500;
// A browser that does not hand over its cookies for the Site Grabber within this is given up on.
constexpr UINT_PTR kCookiesTimer = 2;
constexpr std::chrono::seconds kCookiesWait{10};

std::wstring WindowTitle()
{
    return L"Last Download Manager " LDM_VERSION_WIDE;
}

// "left,top,right,bottom,showCommand,treeWidth" in the coordinates of SetWindowPlacement.
std::string SerializePlacement(const WINDOWPLACEMENT& placement, int treeWidth96)
{
    const RECT& r = placement.rcNormalPosition;
    char text[128] = {};
    std::snprintf(text, sizeof(text), "%ld,%ld,%ld,%ld,%u,%d", r.left, r.top, r.right, r.bottom, placement.showCmd,
                  treeWidth96);
    return text;
}

bool ParsePlacement(const std::string& text, RECT& rect, UINT& showCommand, int& treeWidth96)
{
    long left = 0;
    long top = 0;
    long right = 0;
    long bottom = 0;
    unsigned show = 0;
    int tree = 0;
    if (sscanf_s(text.c_str(), "%ld,%ld,%ld,%ld,%u,%d", &left, &top, &right, &bottom, &show, &tree) != 6) {
        return false;
    }
    if (right - left < 200 || bottom - top < 150) {
        return false;
    }
    rect = RECT{left, top, right, bottom};
    showCommand = show;
    treeWidth96 = std::clamp(tree, kMinimumTreeWidth96, 800);
    return true;
}

}  // namespace

MainWindow::MainWindow(const Services& services) : services_(services)
{
    if (services_.browser != nullptr) {
        BrowserSessions::Handlers handlers;
        // Captures and links arrive on the UI thread; they are queued so they wait for any open
        // dialog like other input.
        handlers.onCapture = [this](browser::Capture capture) { QueueExternalInput(std::move(capture)); };
        handlers.onLinks = [this](BrowserSessions::LinksRequest request) {
            if (controller_) {
                controller_->Adds().ExpectLinks(request.connection);
            }
            QueueExternalInput(std::move(request));
        };
        handlers.onLinkInfo = [this](BrowserConnectionId connection, browser::LinkInfo info) {
            if (controller_) {
                controller_->Adds().OnLinkInfo(connection, std::move(info));
            }
        };
        handlers.onClosed = [this](BrowserConnectionId connection) {
            if (controller_) {
                controller_->Adds().OnBrowserClosed(connection);
            }
            if (mediaPanel_) {
                mediaPanel_->OnConnectionClosed(connection);
            }
        };
        handlers.onMedia = [this](BrowserConnectionId connection, browser::MediaFound media) {
            if (mediaPanel_) {
                mediaPanel_->OnMedia(connection, std::move(media));
            }
        };
        handlers.onMediaReset = [this](BrowserConnectionId connection, int tabId) {
            if (mediaPanel_) {
                mediaPanel_->OnMediaReset(connection, tabId);
            }
        };
        handlers.onSitePage = [this](BrowserConnectionId connection, browser::SitePage page) {
            if (mediaPanel_) {
                mediaPanel_->OnSitePage(connection, std::move(page));
            }
        };
        handlers.onVideoPosition = [this](BrowserConnectionId connection, browser::VideoPosition position) {
            if (mediaPanel_) {
                mediaPanel_->OnVideoPosition(connection, position);
            }
        };
        browserSessions_ = std::make_unique<BrowserSessions>(*services_.browser, base::ToUtf8(LDM_VERSION_WIDE),
                                                             std::move(handlers));
    }
}

MainWindow::~MainWindow()
{
    if (window_ != nullptr && ::IsWindow(window_)) {
        ::SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
        ::DestroyWindow(window_);
    }
}

bool MainWindow::Create(std::shared_ptr<const core::Snapshot> snapshot, int showCommand)
{
    snapshot_ = std::move(snapshot);
    initialShowCommand_ = showCommand;
    theme_ = std::make_unique<Theme>(snapshot_->settings.darkModeSupport);

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = &MainWindow::WindowProc;
    windowClass.hInstance = ::GetModuleHandleW(nullptr);
    windowClass.hIcon = ::LoadIconW(windowClass.hInstance, MAKEINTRESOURCEW(IDI_LDM));
    windowClass.hIconSm = static_cast<HICON>(::LoadImageW(windowClass.hInstance, MAKEINTRESOURCEW(IDI_LDM), IMAGE_ICON,
                                                          ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), 0));
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kClassName;
    windowClass.lpszMenuName = MAKEINTRESOURCEW(IDR_MAIN_MENU);
    ::RegisterClassExW(&windowClass);
    accelerators_ = ::LoadAcceleratorsW(windowClass.hInstance, MAKEINTRESOURCEW(IDR_ACCELERATORS));

    window_ = ::CreateWindowExW(0, kClassName, WindowTitle().c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, 1000, 640, nullptr, nullptr, windowClass.hInstance, this);
    if (window_ != nullptr && services_.elevated && services_.browser != nullptr) {
        QueueExternalInput(ElevationNotice{});
    }
    return window_ != nullptr;
}

LRESULT CALLBACK MainWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        auto* self = static_cast<MainWindow*>(create->lpCreateParams);
        self->window_ = window;
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<MainWindow*>(::GetWindowLongPtrW(window, GWLP_USERDATA));  // stored at WM_NCCREATE
    if (self == nullptr) {
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
    return self->HandleMessage(message, wParam, lParam);
}

bool MainWindow::PreTranslate(MSG& message)
{
    if (controller_ && controller_->PreTranslate(message)) {
        return true;
    }
    if (grabber_ && grabber_->PreTranslate(message)) {
        return true;
    }
    if (window_ != nullptr && accelerators_ != nullptr && (message.hwnd == window_ || ::IsChild(window_, message.hwnd))) {
        return ::TranslateAcceleratorW(window_, accelerators_, &message) != 0;
    }
    return false;
}

void MainWindow::OnCreate()
{
    const model::Settings& settings = snapshot_->settings;
    toolbar_.Create(window_, kToolbarId, settings.largeToolbarButtons, settings.toolbarLayout, *theme_);
    toolbar_.SetDropDownHandler([this](int command, POINT screen) { controller_->ShowQueueToolbarMenu(command, screen); });
    categories_.Create(window_, CategoryPane::Callbacks{
                                    [this](const ListFilter& filter) { list_.SetFilter(filter); },
                                    [this] {
                                        services_.commands->Post(core::ModifySettingsCommand{
                                            [](model::Settings& s) { s.hideCategories = true; }});
                                    },
                                    [this](std::optional<ListFilter> node, POINT screen) {
                                        if (node && node->status == StatusFilter::Queued) {
                                            controller_->ShowQueueMenu(node->queue, screen);
                                        } else {
                                            controller_->ShowCategoryMenu(node ? node->category : std::nullopt, screen);
                                        }
                                    }});
    list_.Create(window_, DownloadListView::Callbacks{
                              [this] { controller_->OnSelectionChanged(); },
                              [this] { controller_->OnCommand(ID_ROW_DEFAULT_ACTION); },
                              [this](POINT screen) { controller_->ShowRowMenu(screen); },
                              [this] { ::PostMessageW(window_, kSaveLayoutMessage, 0, 0); }});
    status_.Create(window_, *theme_);
    CreateFonts();  // after the children exist, so each one receives its font

    MainController::Context context;
    context.owner = window_;
    context.commands = services_.commands;
    context.shell = services_.shell;
    context.system = services_.system;
    context.secrets = services_.secrets;
    context.startup = services_.startup;
    context.columnsChanged = [this](const std::vector<ColumnState>&) { ::PostMessageW(window_, kSaveLayoutMessage, 0, 0); };
    context.list = &list_;
    context.toolbar = &toolbar_;
    context.theme = theme_.get();
    context.snapshot = [this] { return snapshot_; };
    context.exit = [this] { Exit(); };
    context.restore = [this] { Restore(); };
    context.currentFilter = [this] { return categories_.Selected(); };
    context.connectedBrowsers = [this] {
        std::vector<browser::Hello> browsers;
        if (browserSessions_) {
            for (const auto& [connection, session] : browserSessions_->Sessions()) {
                if (session.hello) {
                    browsers.push_back(*session.hello);
                }
            }
        }
        return browsers;
    };
    context.linksDone = [this](BrowserConnectionId connection, const std::string& requestId) {
        if (browserSessions_) {
            browserSessions_->LinksDone(connection, requestId);
        }
    };
    context.openPage = [this](const std::string& url) {
        return browserSessions_ && browserSessions_->OpenPage(url);
    };
    if (services_.grabber != nullptr && services_.grabberBridge != nullptr) {
        context.grabber = services_.grabber;
        context.grabberSnapshot = [this] { return grabberSnapshot_; };
    }
    controller_ = std::make_unique<MainController>(std::move(context));
    if (browserSessions_ && services_.browserWindows != nullptr && services_.readXml) {
        CreateMediaPanel();
    }
    if (services_.grabber != nullptr && services_.grabberBridge != nullptr) {
        GrabberController::Context grabber;
        grabber.owner = window_;
        grabber.grabber = services_.grabber;
        grabber.shell = services_.shell;
        grabber.theme = theme_.get();
        grabber.snapshot = [this] { return snapshot_; };
        grabber.requestCookies = [this](const std::string& url, GrabberController::CookiesAnswer answer) {
            if (!browserSessions_ ||
                !browserSessions_->RequestCookies(url, std::move(answer), std::chrono::steady_clock::now() + kCookiesWait)) {
                return false;
            }
            ::SetTimer(window_, kCookiesTimer,
                       static_cast<UINT>(std::chrono::milliseconds(kCookiesWait).count()) + 100, nullptr);
            return true;
        };
        grabber_ = std::make_unique<GrabberController>(std::move(grabber));
    }

    list_.SetColumns(ParseColumns(settings.columnLayout));
    list_.SetSort(ParseSort(settings.listSortKey));
    ApplyViewSettings(settings, true);
    ApplyTheme();
    tray_.SetStyle(settings.trayIconStyle);
    tray_.Attach(window_);
    ApplySnapshot(snapshot_);
    services_.bridge->Attach(window_);
    if (grabber_) {
        services_.grabberBridge->Attach(window_);
    }
    ::AddClipboardFormatListener(window_);
    // Links dropped anywhere on the main window are added like drops on the drop target.
    (void)linkDrop_.Register(window_, LinkDropTarget::Callbacks{
                                          [this](std::vector<std::string> urls) { QueueDroppedUrls(std::move(urls)); }, {}});
}

void MainWindow::CreateFonts()
{
    const UINT dpi = DpiOf(window_);
    uiFont_ = CreateMessageFont(dpi);
    listFont_ = CreateFontFromSetting(base::ToUtf16(snapshot_->settings.listFont), dpi);
    categories_.SetFont(uiFont_.get());
    status_.SetFont(uiFont_.get());
    list_.SetFont(listFont_.get());
    if (toolbar_.Handle() != nullptr) {
        ::SendMessageW(toolbar_.Handle(), WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_.get()), TRUE);
    }
}

void MainWindow::ApplyTheme()
{
    theme_->ApplyToTopLevel(window_);
    categories_.ApplyTheme(*theme_);
    list_.ApplyTheme(*theme_);
    status_.SetTheme(*theme_);
    toolbar_.Refresh(snapshot_->settings.largeToolbarButtons, *theme_);
    ::SetClassLongPtrW(window_, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(theme_->PanelBrush()));
    ::DrawMenuBar(window_);
    ::RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
    Layout();
    if (mediaPanel_) {
        mediaPanel_->ThemeChanged();
    }
}

void MainWindow::CreateMediaPanel()
{
    MediaPanelController::Hooks hooks;
    hooks.fetch = [this](core::FetchManifestCommand command) { services_.commands->Post(std::move(command)); };
    hooks.sendMediaList = [this](BrowserConnectionId connection, int tabId, bool hasMedia) {
        browserSessions_->SendMediaList(connection, tabId, hasMedia);
    };
    hooks.hostProcessOf = [this](BrowserConnectionId connection) -> std::uint32_t {
        const auto& sessions = browserSessions_->Sessions();
        const auto session = sessions.find(connection);
        return session == sessions.end() ? 0 : session->second.hostProcessId;
    };
    // Plain files from the panel go through the add flow like a browser capture.
    hooks.downloadFile = [this](browser::Capture capture) { QueueExternalInput(std::move(capture)); };
    hooks.downloadStream = [this](StreamCapture capture) { QueueExternalInput(std::move(capture)); };
    hooks.tell = [this](const std::wstring& text) {
        (void)ShowMessage(window_, *theme_, MessageRequest{text, MessageKind::Information});
    };
    hooks.readXml = services_.readXml;
    hooks.lookUp = [this](core::LookUpSiteVideoCommand command) { services_.commands->Post(std::move(command)); };
    hooks.cancelLookup = [this](std::int64_t lookupId) { services_.commands->Post(core::CancelSiteLookupCommand{lookupId}); };
    mediaPanel_ = std::make_unique<MediaPanelController>(*services_.browserWindows, *theme_, std::move(hooks));
}

void MainWindow::ApplyViewSettings(const model::Settings& settings, bool initial)
{
    const model::Settings* previous = appliedSettings_ ? &*appliedSettings_ : nullptr;
    if (initial) {
        RestorePlacement(settings.windowPlacement, initialShowCommand_);
    }
    if (previous == nullptr || previous->hideCategories != settings.hideCategories) {
        categoriesHidden_ = settings.hideCategories;
        categories_.Show(!categoriesHidden_);
        Layout();
    }
    if (previous != nullptr && previous->largeToolbarButtons != settings.largeToolbarButtons) {
        toolbar_.Refresh(settings.largeToolbarButtons, *theme_);
        Layout();
    }
    if (previous == nullptr || previous->showDropTarget != settings.showDropTarget) {
        UpdateDropTarget(settings);
    }
    if (previous != nullptr && previous->trayIconStyle != settings.trayIconStyle) {
        tray_.SetStyle(settings.trayIconStyle);
    }
    if (previous != nullptr && previous->darkModeSupport != settings.darkModeSupport) {
        if (theme_->Refresh(settings.darkModeSupport)) {
            ApplyTheme();
        }
    }
    appliedSettings_ = settings;
}

void MainWindow::UpdateDropTarget(const model::Settings& settings)
{
    if (!settings.showDropTarget) {
        dropTarget_.Destroy();
        return;
    }
    if (dropTarget_.IsOpen()) {
        return;
    }
    DropTargetWindow::Callbacks callbacks;
    // Dropping runs inside the source program's drag loop: queue the addresses and return at once,
    // so a dialog never holds the browser's drag and drop hostage.
    callbacks.dropped = [this](std::vector<std::string> urls) { QueueDroppedUrls(std::move(urls)); };
    callbacks.hideRequested = [this] {
        services_.commands->Post(core::ModifySettingsCommand{[](model::Settings& s) { s.showDropTarget = false; }});
    };
    callbacks.openMain = [this] { Restore(); };
    callbacks.moved = [this](const std::string& position) {
        services_.commands->Post(core::ModifySettingsCommand{[position](model::Settings& s) { s.dropTargetPosition = position; }});
    };
    dropTarget_.Create(settings.dropTargetPosition, *theme_, std::move(callbacks));
}

void MainWindow::QueueExternalInput(ExternalInput input)
{
    {
        const std::lock_guard lock(externalMutex_);
        externalInput_.push_back(std::move(input));
    }
    ::PostMessageW(window_, kExternalInputMessage, 0, 0);
}

void MainWindow::QueueDroppedUrls(std::vector<std::string> urls)
{
    // Dropping runs inside the source program's drag loop: queue the addresses and return at once,
    // so a dialog never holds the browser's drag and drop hostage.
    QueueExternalInput(std::move(urls));
}

void MainWindow::PostLaunchRequest(core::LaunchRequest request)
{
    QueueExternalInput(std::move(request));
}

void MainWindow::PostBrowserEvent(BrowserEvent event)
{
    {
        const std::lock_guard lock(browserMutex_);
        browserEvents_.push_back(std::move(event));
    }
    ::PostMessageW(window_, kBrowserEventMessage, 0, 0);
}

void MainWindow::DrainBrowserEvents()
{
    std::deque<BrowserEvent> events;
    {
        const std::lock_guard lock(browserMutex_);
        events.swap(browserEvents_);
    }
    if (!browserSessions_ || !snapshot_) {
        return;
    }
    for (BrowserEvent& event : events) {
        browserSessions_->OnEvent(std::move(event), snapshot_->settings);
    }
}

void MainWindow::DrainExternalInput()
{
    if (draining_ || !controller_) {
        return;  // the outer call picks up anything queued meanwhile
    }
    draining_ = true;
    for (;;) {
        if (!::IsWindowEnabled(window_)) {
            // A modal dialog is open; try again shortly instead of stacking dialogs.
            ::SetTimer(window_, kExternalInputRetryTimer, kExternalInputRetryMs, nullptr);
            break;
        }
        std::optional<ExternalInput> next;
        {
            const std::lock_guard lock(externalMutex_);
            if (externalInput_.empty()) {
                break;
            }
            next = std::move(externalInput_.front());
            externalInput_.pop_front();
        }
        if (auto* request = std::get_if<core::LaunchRequest>(&*next)) {
            controller_->OnLaunchRequest(*request);
        } else if (auto* capture = std::get_if<browser::Capture>(&*next)) {
            controller_->OnBrowserCapture(std::move(*capture));
        } else if (auto* links = std::get_if<BrowserSessions::LinksRequest>(&*next)) {
            controller_->OnBrowserLinks(std::move(*links));
        } else if (std::holds_alternative<ElevationNotice>(*next)) {
            controller_->WarnElevated();
        } else if (std::holds_alternative<UpdatesQuestion>(*next)) {
            controller_->AskAboutUpdatesOnce();
        } else if (auto* stream = std::get_if<StreamCapture>(&*next)) {
            controller_->OnStreamCapture(std::move(*stream));
        } else {
            controller_->OnDroppedUrls(std::move(std::get<std::vector<std::string>>(*next)));
        }
    }
    draining_ = false;
}

void MainWindow::RestorePlacement(const std::string& placement, int showCommand)
{
    RECT rect{};
    UINT storedShow = SW_SHOWNORMAL;
    int treeWidth = treeWidth96_;
    if (ParsePlacement(placement, rect, storedShow, treeWidth)) {
        treeWidth96_ = treeWidth;
        WINDOWPLACEMENT data{sizeof(data)};
        data.rcNormalPosition = rect;
        data.showCmd = SW_HIDE;
        ::SetWindowPlacement(window_, &data);
        if (storedShow == SW_SHOWMAXIMIZED && showCommand == SW_SHOWNORMAL) {
            initialShowCommand_ = SW_SHOWMAXIMIZED;
        }
    }
}

void MainWindow::SaveViewState()
{
    WINDOWPLACEMENT placement{sizeof(placement)};
    ::GetWindowPlacement(window_, &placement);
    const std::string stored = SerializePlacement(placement, treeWidth96_);
    const std::string columns = SerializeColumns(list_.Columns());
    const std::string sort = SerializeSort(list_.Sort());
    const std::string toolbar = toolbar_.Layout();
    services_.commands->Post(core::ModifySettingsCommand{[stored, columns, sort, toolbar](model::Settings& s) {
        s.windowPlacement = stored;
        s.columnLayout = columns;
        s.listSortKey = sort;
        s.toolbarLayout = toolbar;
    }});
}

MainWindow::PaneRects MainWindow::Panes() const
{
    RECT client{};
    ::GetClientRect(window_, &client);
    const UINT dpi = DpiOf(window_);
    const int margin = Scale(kMargin96, dpi);
    PaneRects panes;
    panes.toolbarBottom = toolbar_.PreferredHeight();
    const int top = panes.toolbarBottom + kDividerPx + Scale(kTopGap96, dpi);
    const int bottom = std::max<int>(top, client.bottom - status_.PreferredHeight() - margin);
    const int right = std::max(margin, static_cast<int>(client.right) - margin);
    panes.categoriesVisible = !categoriesHidden_;
    if (!panes.categoriesVisible) {
        panes.listFrame = RECT{margin, top, right, bottom};
        return panes;
    }
    const int maxTree = std::max(Scale(kMinimumTreeWidth96, dpi), right - margin - Scale(kMinimumListWidth96, dpi));
    const int treeWidth = std::clamp(Scale(treeWidth96_, dpi), Scale(kMinimumTreeWidth96, dpi), maxTree);
    const int splitter = Scale(kSplitterWidth96, dpi);
    panes.categoryFrame = RECT{margin, top, margin + treeWidth, bottom};
    panes.splitter = RECT{panes.categoryFrame.right, top, panes.categoryFrame.right + splitter, bottom};
    panes.listFrame = RECT{panes.splitter.right, top, std::max<int>(panes.splitter.right, right), bottom};
    return panes;
}

RECT MainWindow::SplitterRect() const
{
    return Panes().splitter;
}

void MainWindow::Layout()
{
    if (window_ == nullptr || toolbar_.Handle() == nullptr || list_.Handle() == nullptr) {
        return;
    }
    // The frames and gaps between children move with the layout.
    ::InvalidateRect(window_, nullptr, FALSE);
    RECT client{};
    ::GetClientRect(window_, &client);
    const int toolbarHeight = toolbar_.PreferredHeight();
    const int statusHeight = status_.PreferredHeight();
    ::MoveWindow(toolbar_.Handle(), 0, 0, client.right, toolbarHeight, TRUE);
    ::MoveWindow(status_.Handle(), 0, client.bottom - statusHeight, client.right, statusHeight, TRUE);

    const PaneRects panes = Panes();
    // The list sits inside a one pixel border; the category pane inside a border and one pixel of padding.
    RECT list = panes.listFrame;
    ::InflateRect(&list, -kDividerPx, -kDividerPx);
    ::MoveWindow(list_.Handle(), list.left, list.top, std::max(0L, list.right - list.left), std::max(0L, list.bottom - list.top),
                 TRUE);
    list_.FitLastColumn();
    if (panes.categoriesVisible) {
        RECT inner = panes.categoryFrame;
        ::InflateRect(&inner, -2 * kDividerPx, -2 * kDividerPx);
        categories_.Layout(inner);
    }
}

void MainWindow::Paint()
{
    PAINTSTRUCT paint{};
    const HDC dc = ::BeginPaint(window_, &paint);
    // Children clip themselves (WS_CLIPCHILDREN): this only reaches the margins and frames, which
    // the class brush already filled with the panel color.
    const PaneRects panes = Panes();
    RECT client{};
    ::GetClientRect(window_, &client);
    const Palette& colors = theme_->Colors();
    UniqueBrush line(::CreateSolidBrush(colors.border));
    const RECT underToolbar{0, panes.toolbarBottom, client.right, panes.toolbarBottom + kDividerPx};
    ::FillRect(dc, &underToolbar, line.get());
    UniqueBrush listBorder(::CreateSolidBrush(colors.listBorder));
    ::FrameRect(dc, &panes.listFrame, listBorder.get());
    if (panes.categoriesVisible) {
        UniqueBrush frame(::CreateSolidBrush(colors.divider));
        ::FrameRect(dc, &panes.categoryFrame, frame.get());
    }
    ::EndPaint(window_, &paint);
}

void MainWindow::ApplySnapshot(std::shared_ptr<const core::Snapshot> snapshot)
{
    if (!snapshot) {
        return;
    }
    snapshot_ = std::move(snapshot);
    categories_.SetCategories(snapshot_->categories);
    std::vector<model::Queue> queues;
    for (const core::QueueView& view : snapshot_->queues) {
        queues.push_back(view.queue);
    }
    categories_.SetQueues(queues);
    list_.SetSnapshot(snapshot_);
    ApplyViewSettings(snapshot_->settings, false);
    controller_->OnSnapshot(*snapshot_);
    if (grabber_) {
        grabber_->OnCoreSnapshot(*snapshot_);
    }
    if (browserSessions_) {
        browserSessions_->OnSettingsChanged(snapshot_->settings);
    }
    if (mediaPanel_) {
        mediaPanel_->ApplySettings(snapshot_->settings);
    }
    UpdateStatus();
}

void MainWindow::UpdateStatus()
{
    const core::Snapshot& snapshot = *snapshot_;
    std::vector<std::wstring> parts;
    const auto listed = std::count_if(snapshot.downloads.begin(), snapshot.downloads.end(),
                                      [](const auto& view) { return view->download.InMainList(); });
    parts.push_back(std::to_wstring(listed) + (listed == 1 ? L" download" : L" downloads"));
    parts.push_back(std::to_wstring(snapshot.activeDownloads) + L" active");
    if (snapshot.totalBytesPerSecond > 0) {
        parts.push_back(L"Speed: " + FormatSpeed(snapshot.totalBytesPerSecond));
    }
    if (snapshot.speedLimiterOn) {
        parts.push_back(L"Speed limiter: " + std::to_wstring(snapshot.settings.speedLimiterKilobytesPerSecond) + L" KB/s");
    }
    if (snapshot.quotaEndsAtUnixMs) {
        parts.push_back(L"Quota used up; continues at " + FormatDateTime(*snapshot.quotaEndsAtUnixMs));
    } else if (snapshot.quotaLimitBytes > 0) {
        parts.push_back(L"Quota: " + FormatSize(snapshot.quotaUsedBytes) + L" of " + FormatSize(snapshot.quotaLimitBytes));
    }
    status_.SetParts(std::move(parts));

    std::wstring tooltip = L"Last Download Manager";
    if (snapshot.activeDownloads > 0) {
        tooltip += L"\n" + std::to_wstring(snapshot.activeDownloads) + L" active, " + FormatSpeed(snapshot.totalBytesPerSecond);
    }
    tray_.SetTooltip(tooltip);
}

void MainWindow::Restore()
{
    if (::IsIconic(window_)) {
        ::ShowWindow(window_, SW_RESTORE);
    } else {
        ::ShowWindow(window_, SW_SHOW);
    }
    ::SetForegroundWindow(window_);
    QueueUpdatesQuestion();
}

void MainWindow::QueueUpdatesQuestion()
{
    // Asked when the user first sees the window, not from the tray at sign-in.
    if (updatesQuestionQueued_ || !snapshot_ || snapshot_->settings.updatesAsked || !::IsWindowVisible(window_)) {
        return;
    }
    updatesQuestionQueued_ = true;
    QueueExternalInput(UpdatesQuestion{});
}

void MainWindow::Exit()
{
    exiting_ = true;
    ::PostMessageW(window_, WM_CLOSE, 0, 0);
}

void MainWindow::OnTrayEvent(UINT event, POINT anchor)
{
    switch (event) {
    case NIN_SELECT:
    case NIN_KEYSELECT:
    case WM_LBUTTONDBLCLK:
        Restore();
        break;
    case WM_CONTEXTMENU:
        controller_->ShowTrayMenu(anchor);
        break;
    default:
        break;
    }
}

LRESULT MainWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    if (theme_) {
        LRESULT result = 0;
        if (theme_->HandleMenuBarMessage(window_, message, wParam, lParam, result)) {
            return result;
        }
    }
    if (message == TrayIcon::TaskbarCreatedMessage() && message != 0) {
        tray_.Recreate();
        return 0;
    }
    switch (message) {
    case WM_CREATE:
        OnCreate();
        ::ShowWindow(window_, initialShowCommand_);
        QueueUpdatesQuestion();
        return 0;
    case WM_SIZE:
        Layout();
        return 0;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_DRAWITEM:
        if (categories_.DrawItem(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
            return TRUE;
        }
        break;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = window_ != nullptr ? DpiOf(window_) : USER_DEFAULT_SCREEN_DPI;
        info->ptMinTrackSize = POINT{Scale(480, dpi), Scale(300, dpi)};
        return 0;
    }
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        CreateFonts();
        toolbar_.Refresh(snapshot_->settings.largeToolbarButtons, *theme_);
        categories_.ApplyTheme(*theme_);
        list_.SetColumns(list_.Columns());
        ::SetWindowPos(window_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                       suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lParam != 0 && std::wstring_view(reinterpret_cast<const wchar_t*>(lParam)) == L"ImmersiveColorSet") {
            if (theme_->Refresh(snapshot_->settings.darkModeSupport)) {
                ApplyTheme();
            }
        }
        return 0;
    case WM_COMMAND: {
        const HWND control = reinterpret_cast<HWND>(lParam);
        if (control != nullptr && categories_.OwnsControl(control) && categories_.HandleCommand(control)) {
            return 0;
        }
        if (LOWORD(wParam) == ID_TASKS_GRABBER) {
            if (grabber_) {
                grabber_->OpenChooser();
            }
            return 0;
        }
        if (controller_ && controller_->OnCommand(LOWORD(wParam))) {
            return 0;
        }
        break;
    }
    case WM_NOTIFY: {
        const auto& header = *reinterpret_cast<const NMHDR*>(lParam);
        LRESULT result = 0;
        bool layoutChanged = false;
        if (toolbar_.HandleNotify(header, result, layoutChanged)) {
            if (layoutChanged) {
                Layout();
                ::PostMessageW(window_, kSaveLayoutMessage, 0, 0);
            }
            return result;
        }
        if (header.code == TTN_GETDISPINFOW) {
            break;
        }
        if (list_.HandleNotify(header, result) || categories_.HandleNotify(header, result)) {
            return result;
        }
        break;
    }
    case WM_INITMENUPOPUP:
        if (controller_) {
            controller_->OnMenuPopup(reinterpret_cast<HMENU>(wParam));
        }
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        if (const HBRUSH brush = categories_.OnCtlColor(reinterpret_cast<HWND>(lParam), reinterpret_cast<HDC>(wParam))) {
            return reinterpret_cast<LRESULT>(brush);
        }
        if (theme_) {
            return reinterpret_cast<LRESULT>(theme_->OnCtlColor(reinterpret_cast<HDC>(wParam), false));
        }
        break;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && !categoriesHidden_) {
            POINT cursor{};
            ::GetCursorPos(&cursor);
            ::ScreenToClient(window_, &cursor);
            const RECT splitter = SplitterRect();
            if (::PtInRect(&splitter, cursor)) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONDOWN: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const RECT splitter = SplitterRect();
        if (!categoriesHidden_ && ::PtInRect(&splitter, point)) {
            draggingSplitter_ = true;
            ::SetCapture(window_);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (draggingSplitter_) {
            const UINT dpi = DpiOf(window_);
            treeWidth96_ = std::max(kMinimumTreeWidth96, Unscale(GET_X_LPARAM(lParam) - Scale(kMargin96, dpi), dpi));
            Layout();
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (draggingSplitter_) {
            draggingSplitter_ = false;
            if (message == WM_LBUTTONUP) {
                ::ReleaseCapture();
            }
            ::PostMessageW(window_, kSaveLayoutMessage, 0, 0);
        }
        return 0;
    case kSaveLayoutMessage:
        // Posted after a column drag, once the list has applied the new order.
        list_.FitLastColumn();
        SaveViewState();
        return 0;
    case kExternalInputMessage:
        DrainExternalInput();
        return 0;
    case kBrowserEventMessage:
        DrainBrowserEvents();
        return 0;
    case WM_TIMER:
        if (wParam == kExternalInputRetryTimer) {
            ::KillTimer(window_, kExternalInputRetryTimer);
            DrainExternalInput();
            return 0;
        }
        if (wParam == kCookiesTimer) {
            ::KillTimer(window_, kCookiesTimer);
            if (browserSessions_) {
                browserSessions_->ExpireCookieRequests(std::chrono::steady_clock::now());
            }
            return 0;
        }
        break;
    case WM_CLIPBOARDUPDATE:
        if (controller_) {
            controller_->OnClipboardChanged();
        }
        return 0;
    case kSnapshotMessage:
        ApplySnapshot(services_.bridge->TakeSnapshot());
        return 0;
    case kNotificationMessage: {
        auto notifications = services_.bridge->TakeNotifications();
        if (mediaPanel_) {
            // Manifest fetches and site lookups answer the media panel, not the download list.
            std::erase_if(notifications, [this](const core::Notification& notification) {
                if (const auto* looked = std::get_if<core::SiteLookupFinished>(&notification)) {
                    return mediaPanel_->OnSiteLookedUp(*looked);
                }
                const auto* fetched = std::get_if<core::FetchFinished>(&notification);
                return fetched != nullptr && mediaPanel_->OnFetched(*fetched);
            });
        }
        controller_->OnNotifications(std::move(notifications));
        return 0;
    }
    case kGrabberSnapshotMessage:
        if (grabber_) {
            grabberSnapshot_ = services_.grabberBridge->TakeSnapshot();
            grabber_->OnSnapshot(grabberSnapshot_);
            controller_->OnGrabberSnapshot(grabberSnapshot_);
        }
        return 0;
    case kGrabberNotificationMessage:
        if (grabber_) {
            grabber_->OnNotifications(services_.grabberBridge->TakeNotifications());
        }
        return 0;
    case kTrayMessage:
        OnTrayEvent(LOWORD(lParam), POINT{GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)});
        return 0;
    case WM_CLOSE:
        // The close button keeps LDM running in the tray; Exit really quits.
        if (!exiting_ && snapshot_ && snapshot_->settings.trayIconStyle != model::TrayIconStyle::Hidden) {
            ::ShowWindow(window_, SW_HIDE);
            return 0;
        }
        SaveViewState();
        ::DestroyWindow(window_);
        return 0;
    case WM_POWERBROADCAST:
        // Scheduled queues: the computer may have slept through, or woken for, a start.
        if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
            services_.commands->Post(core::ClockChangedCommand{});
        }
        return TRUE;
    case WM_TIMECHANGE:
        services_.commands->Post(core::ClockChangedCommand{});
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wParam != 0) {
            exiting_ = true;
            SaveViewState();
            ::DestroyWindow(window_);
        }
        return 0;
    case WM_DESTROY:
        ::RemoveClipboardFormatListener(window_);
        ::KillTimer(window_, kExternalInputRetryTimer);
        linkDrop_.Revoke();
        dropTarget_.Destroy();
        services_.bridge->Attach(nullptr);
        ::KillTimer(window_, kCookiesTimer);
        if (grabber_) {
            services_.grabberBridge->Attach(nullptr);
            grabber_->CloseAll();
        }
        ::PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(window_, message, wParam, lParam);
}

}  // namespace ldm::ui
