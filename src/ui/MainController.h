#pragma once

#include "browser/Protocol.h"
#include "core/Commands.h"
#include "core/LaunchRequest.h"
#include "core/Ports.h"
#include "core/Snapshot.h"
#include "ui/AddFlow.h"
#include "ui/BrowserSessions.h"
#include "ui/CaptureFollowUp.h"
#include "ui/ClipboardMonitor.h"
#include "ui/DownloadListView.h"
#include "ui/ImportExportFlow.h"
#include "ui/PlaylistOffer.h"
#include "ui/ListModel.h"
#include "ui/QueueCommands.h"
#include "ui/ShellActions.h"
#include "ui/StartupRegistration.h"
#include "ui/StreamChoice.h"
#include "ui/SystemActions.h"
#include "ui/Theme.h"
#include "ui/Toolbar.h"
#include "ui/TransferWindows.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ldm::ui {

class FindDialog;
class SchedulerWindow;

// Turns menu, toolbar and tray commands into dialogs and core commands (design section 6). Adding
// downloads lives in AddFlow and per-transfer windows in TransferWindows; this class dispatches and
// handles the list, view and category commands. It holds no download state: it reads the newest
// snapshot and posts commands.
class MainController {
public:
    struct Context {
        HWND owner = nullptr;
        core::CommandSink* commands = nullptr;
        ShellActions* shell = nullptr;
        SystemActions* system = nullptr;
        core::SecretProtector* secrets = nullptr;
        StartupRegistration* startup = nullptr;
        DownloadListView* list = nullptr;
        Toolbar* toolbar = nullptr;
        const Theme* theme = nullptr;
        std::function<std::shared_ptr<const core::Snapshot>()> snapshot;
        std::function<void()> exit;
        std::function<void()> restore;
        std::function<ListFilter()> currentFilter;
        std::function<void(const std::vector<ColumnState>&)> columnsChanged;
        // Extensions connected right now, for Options > General; empty without browser integration.
        std::function<std::vector<browser::Hello>()> connectedBrowsers;
        // The Download All Links dialog of a browser request closed.
        std::function<void(BrowserConnectionId, const std::string&)> linksDone;
        // Opens a page in a connected browser (Refresh download address); false without one.
        std::function<bool(const std::string&)> openPage;
    };

    explicit MainController(Context context);
    ~MainController();
    MainController(const MainController&) = delete;
    MainController& operator=(const MainController&) = delete;

    // Returns true when the command was handled.
    bool OnCommand(int id);
    void OnMenuPopup(HMENU menu);
    void OnSelectionChanged();
    void OnSnapshot(const core::Snapshot& snapshot);
    void OnNotifications(std::vector<core::Notification> notifications);
    bool PreTranslate(MSG& message);

    void ShowRowMenu(POINT screen);
    void ShowCategoryMenu(std::optional<model::CategoryId> category, POINT screen);
    // The tree's Queues node (no queue) or one of its queues.
    void ShowQueueMenu(std::optional<model::QueueId> queue, POINT screen);
    // The toolbar's Start Queue or Stop Queue drop-down.
    void ShowQueueToolbarMenu(int command, POINT screen);
    void ShowTrayMenu(POINT screen);

    // A command line or a second launch (checklist A-10, W-05).
    void OnLaunchRequest(const core::LaunchRequest& request);
    // A download the browser extension handed over (M3 design section 5).
    void OnBrowserCapture(browser::Capture capture);
    // A stream picked in the media panel.
    void OnStreamCapture(StreamCapture capture);
    // "Download all links" or "Download selected links" from the browser (checklist B-05).
    void OnBrowserLinks(BrowserSessions::LinksRequest request);
    // WM_CLIPBOARDUPDATE (checklist A-09).
    void OnClipboardChanged();
    // Addresses dropped on the drop target (checklist W-06).
    void OnDroppedUrls(std::vector<std::string> urls);
    // LDM runs as administrator, so browsers cannot hand it downloads (checklist B-10).
    void WarnElevated();

    [[nodiscard]] AddFlow& Adds() noexcept { return adds_; }

private:
    void RemoveSelected();
    void OpenSelected();
    void DefaultAction();
    void ShowProperties();
    void MoveSelected(HWND owner);
    void Find(bool showDialog);
    void CustomizeColumns();
    void EditCategory(std::optional<model::CategoryId> id);
    void DeleteCategory(model::CategoryId id);
    void SpeedLimiterSettings();
    void OpenScheduler(std::optional<model::QueueId> select);
    void ShowOptions();
    void AskForLogin(const core::DownloadFailed& failed);
    void AskAboutQuota(const core::QuotaReached& reached);
    void ChooseListFont();
    void ModifySettings(std::function<void(model::Settings&)> modify);
    void SetSort(SortKey key);
    void Post(core::Command command);
    [[nodiscard]] std::vector<const core::DownloadView*> Selection() const;
    [[nodiscard]] const core::DownloadView* SingleSelection() const;
    [[nodiscard]] std::shared_ptr<const core::Snapshot> Snapshot() const;
    void ShowError(const std::wstring& message) const;
    // A Yes/No question; true for Yes.
    [[nodiscard]] bool Ask(const std::wstring& question) const;
    void Tell(const std::wstring& message) const;

    Context context_;
    TransferWindows transfers_;
    CaptureFollowUp captures_;
    AddFlow adds_;
    ImportExportFlow importExport_;
    PlaylistOffer playlists_;
    QueueCommands queues_;
    ClipboardMonitor clipboard_;
    std::unique_ptr<FindDialog> find_;
    std::unique_ptr<SchedulerWindow> scheduler_;
    FindOptions findOptions_;
    std::optional<model::CategoryId> categoryMenuTarget_;
    std::vector<HWND> trayWindows_;  // download windows listed in the open tray menu
};

}  // namespace ldm::ui
