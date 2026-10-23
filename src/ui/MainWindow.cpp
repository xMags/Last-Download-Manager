#include "ui/MainWindow.h"

#include "base/Utf.h"
#include "resource.h"
#include "ui/Dpi.h"
#include "ui/Format.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>

namespace ldm::ui {

namespace {

constexpr wchar_t kClassName[] = L"LdmMainWindow";
constexpr int kToolbarId = 100;
constexpr int kSplitterWidth96 = 5;
constexpr int kMinimumTreeWidth96 = 120;
constexpr int kMinimumListWidth96 = 200;
constexpr UINT kSaveLayoutMessage = WM_APP + 10;
constexpr UINT kExternalInputMessage = WM_APP + 11;
constexpr UINT_PTR kExternalInputRetryTimer = 1;
constexpr UINT kExternalInputRetryMs = 500;

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

MainWindow::MainWindow(const Services& services) : services_(services) {}

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
    if (window_ != nullptr && accelerators_ != nullptr && (message.hwnd == window_ || ::IsChild(window_, message.hwnd))) {
        return ::TranslateAcceleratorW(window_, accelerators_, &message) != 0;
    }
    return false;
}

void MainWindow::OnCreate()
{
    CreateFonts();
    const model::Settings& settings = snapshot_->settings;
    toolbar_.Create(window_, kToolbarId, settings.largeToolbarButtons, settings.toolbarLayout, *theme_);
    categories_.Create(window_, CategoryPane::Callbacks{
                                    [this](const ListFilter& filter) { list_.SetFilter(filter); },
                                    [this] {
                                        services_.commands->Post(core::ModifySettingsCommand{
                                            [](model::Settings& s) { s.hideCategories = true; }});
                                    },
                                    [this](std::optional<model::CategoryId> category, POINT screen) {
                                        controller_->ShowCategoryMenu(category, screen);
                                    }});
    list_.Create(window_, DownloadListView::Callbacks{
                              [this] { controller_->OnSelectionChanged(); },
                              [this] { controller_->OnCommand(ID_ROW_DEFAULT_ACTION); },
                              [this](POINT screen) { controller_->ShowRowMenu(screen); },
                              [this] { ::PostMessageW(window_, kSaveLayoutMessage, 0, 0); }});
    status_.Create(window_, *theme_);

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
    controller_ = std::make_unique<MainController>(std::move(context));

    list_.SetColumns(ParseColumns(settings.columnLayout));
    list_.SetSort(ParseSort(settings.listSortKey));
    ApplyViewSettings(settings, true);
    ApplyTheme();
    tray_.SetStyle(settings.trayIconStyle);
    tray_.Attach(window_);
    ApplySnapshot(snapshot_);
    services_.bridge->Attach(window_);
    ::AddClipboardFormatListener(window_);
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
    callbacks.dropped = [this](std::vector<std::string> urls) {
        {
            const std::lock_guard lock(externalMutex_);
            externalInput_.emplace_back(std::move(urls));
        }
        ::PostMessageW(window_, kExternalInputMessage, 0, 0);
    };
    callbacks.hideRequested = [this] {
        services_.commands->Post(core::ModifySettingsCommand{[](model::Settings& s) { s.showDropTarget = false; }});
    };
    callbacks.openMain = [this] { Restore(); };
    callbacks.moved = [this](const std::string& position) {
        services_.commands->Post(core::ModifySettingsCommand{[position](model::Settings& s) { s.dropTargetPosition = position; }});
    };
    dropTarget_.Create(settings.dropTargetPosition, *theme_, std::move(callbacks));
}

void MainWindow::PostLaunchRequest(core::LaunchRequest request)
{
    {
        const std::lock_guard lock(externalMutex_);
        externalInput_.emplace_back(std::move(request));
    }
    ::PostMessageW(window_, kExternalInputMessage, 0, 0);
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

RECT MainWindow::SplitterRect() const
{
    RECT client{};
    ::GetClientRect(window_, &client);
    const UINT dpi = DpiOf(window_);
    const int top = toolbar_.PreferredHeight();
    const int bottom = client.bottom - status_.PreferredHeight();
    const int x = Scale(treeWidth96_, dpi);
    return RECT{x, top, x + Scale(kSplitterWidth96, dpi), bottom};
}

void MainWindow::Layout()
{
    if (window_ == nullptr || toolbar_.Handle() == nullptr || list_.Handle() == nullptr) {
        return;
    }
    RECT client{};
    ::GetClientRect(window_, &client);
    const UINT dpi = DpiOf(window_);
    const int toolbarHeight = toolbar_.PreferredHeight();
    const int statusHeight = status_.PreferredHeight();
    ::MoveWindow(toolbar_.Handle(), 0, 0, client.right, toolbarHeight, TRUE);
    ::MoveWindow(status_.Handle(), 0, client.bottom - statusHeight, client.right, statusHeight, TRUE);
    const int top = toolbarHeight;
    const int bottom = client.bottom - statusHeight;
    if (categoriesHidden_) {
        ::MoveWindow(list_.Handle(), 0, top, client.right, bottom - top, TRUE);
        return;
    }
    const int maxTree = std::max(Scale(kMinimumTreeWidth96, dpi), static_cast<int>(client.right) - Scale(kMinimumListWidth96, dpi));
    const int treeWidth = std::clamp(Scale(treeWidth96_, dpi), Scale(kMinimumTreeWidth96, dpi), maxTree);
    const int splitter = Scale(kSplitterWidth96, dpi);
    categories_.Layout(RECT{0, top, treeWidth, bottom});
    ::MoveWindow(list_.Handle(), treeWidth + splitter, top, client.right - treeWidth - splitter, bottom - top, TRUE);
}

void MainWindow::ApplySnapshot(std::shared_ptr<const core::Snapshot> snapshot)
{
    if (!snapshot) {
        return;
    }
    snapshot_ = std::move(snapshot);
    categories_.SetCategories(snapshot_->categories);
    list_.SetSnapshot(snapshot_);
    ApplyViewSettings(snapshot_->settings, false);
    controller_->OnSnapshot(*snapshot_);
    UpdateStatus();
}

void MainWindow::UpdateStatus()
{
    const core::Snapshot& snapshot = *snapshot_;
    std::vector<std::wstring> parts;
    parts.push_back(std::to_wstring(snapshot.downloads.size()) + (snapshot.downloads.size() == 1 ? L" download" : L" downloads"));
    parts.push_back(std::to_wstring(snapshot.activeDownloads) + L" active");
    if (snapshot.totalBytesPerSecond > 0) {
        parts.push_back(L"Speed: " + FormatSpeed(snapshot.totalBytesPerSecond));
    }
    if (snapshot.speedLimiterOn) {
        parts.push_back(L"Speed limiter: " + std::to_wstring(snapshot.settings.speedLimiterKilobytesPerSecond) + L" KB/s");
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
        return 0;
    case WM_SIZE:
        Layout();
        return 0;
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
            treeWidth96_ = std::max(kMinimumTreeWidth96, Unscale(GET_X_LPARAM(lParam), DpiOf(window_)));
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
        SaveViewState();
        return 0;
    case kExternalInputMessage:
        DrainExternalInput();
        return 0;
    case WM_TIMER:
        if (wParam == kExternalInputRetryTimer) {
            ::KillTimer(window_, kExternalInputRetryTimer);
            DrainExternalInput();
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
    case kNotificationMessage:
        controller_->OnNotifications(services_.bridge->TakeNotifications());
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
        dropTarget_.Destroy();
        services_.bridge->Attach(nullptr);
        ::PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(window_, message, wParam, lParam);
}

}  // namespace ldm::ui
