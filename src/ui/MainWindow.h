#pragma once

#include "core/Commands.h"
#include "core/LaunchRequest.h"
#include "core/Snapshot.h"
#include "ui/CategoryPane.h"
#include "ui/DownloadListView.h"
#include "ui/DropTargetWindow.h"
#include "ui/Handles.h"
#include "ui/MainController.h"
#include "ui/ShellActions.h"
#include "ui/StartupRegistration.h"
#include "ui/StatusBar.h"
#include "ui/SystemActions.h"
#include "ui/Theme.h"
#include "ui/Toolbar.h"
#include "ui/TrayIcon.h"
#include "ui/UiBridge.h"

#include <windows.h>

#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ldm::ui {

// The main window (checklist W-01): window lifecycle, layout of toolbar, category pane, list and
// status bar, DPI and theme changes, and the tray. Commands go to MainController.
class MainWindow {
public:
    struct Services {
        core::CommandSink* commands = nullptr;
        UiBridge* bridge = nullptr;
        ShellActions* shell = nullptr;
        SystemActions* system = nullptr;
        core::SecretProtector* secrets = nullptr;
        StartupRegistration* startup = nullptr;
    };

    explicit MainWindow(const Services& services);
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // Creates the window from the first snapshot's settings. Returns false when creation failed.
    [[nodiscard]] bool Create(std::shared_ptr<const core::Snapshot> snapshot, int showCommand);
    [[nodiscard]] HWND Handle() const noexcept { return window_; }

    // Accelerators and modeless dialogs; returns true when the message was consumed.
    bool PreTranslate(MSG& message);

    // Brings the window back from the tray or the taskbar.
    void Restore();
    // Leaves the message loop after saving view state (Tasks > Exit, tray Exit).
    void Exit();

    // Queues a launch request for the UI thread. Safe to call from any thread.
    void PostLaunchRequest(core::LaunchRequest request);

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void OnCreate();
    void ApplySnapshot(std::shared_ptr<const core::Snapshot> snapshot);
    void ApplyViewSettings(const model::Settings& settings, bool initial);
    void ApplyTheme();
    void CreateFonts();
    void Layout();
    void SaveViewState();
    void RestorePlacement(const std::string& placement, int showCommand);
    void OnTrayEvent(UINT event, POINT anchor);
    void UpdateStatus();
    [[nodiscard]] RECT SplitterRect() const;
    void UpdateDropTarget(const model::Settings& settings);
    // Where the framed panes go, in client coordinates (the reference's layout: grey margins, framed panes).
    struct PaneRects {
        int toolbarBottom = 0;
        bool categoriesVisible = false;
        RECT categoryFrame{};
        RECT splitter{};
        RECT listFrame{};
    };
    [[nodiscard]] PaneRects Panes() const;
    // The line under the toolbar and the pane frames.
    void Paint();
    // Handles queued launch requests and drops one at a time, waiting while a modal dialog is open.
    void DrainExternalInput();

    // Input that arrives from outside the window and may open dialogs.
    using ExternalInput = std::variant<core::LaunchRequest, std::vector<std::string>>;

    Services services_;
    HWND window_ = nullptr;
    HACCEL accelerators_ = nullptr;
    std::unique_ptr<Theme> theme_;
    std::shared_ptr<const core::Snapshot> snapshot_;
    std::optional<model::Settings> appliedSettings_;

    Toolbar toolbar_;
    CategoryPane categories_;
    DownloadListView list_;
    StatusBar status_;
    TrayIcon tray_;
    DropTargetWindow dropTarget_;
    std::unique_ptr<MainController> controller_;

    UniqueFont uiFont_;
    UniqueFont listFont_;
    int treeWidth96_ = 200;
    bool categoriesHidden_ = false;
    bool draggingSplitter_ = false;
    bool exiting_ = false;
    int initialShowCommand_ = SW_SHOWNORMAL;
    bool draining_ = false;

    std::mutex externalMutex_;  // guards externalInput_; launch requests arrive on the pipe thread
    std::deque<ExternalInput> externalInput_;
};

}  // namespace ldm::ui
