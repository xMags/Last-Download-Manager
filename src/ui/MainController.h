#pragma once

#include "core/Commands.h"
#include "core/Ports.h"
#include "core/Snapshot.h"
#include "ui/AddFlow.h"
#include "ui/DownloadListView.h"
#include "ui/ListModel.h"
#include "ui/ShellActions.h"
#include "ui/StartupRegistration.h"
#include "ui/SystemActions.h"
#include "ui/Theme.h"
#include "ui/Toolbar.h"
#include "ui/TransferWindows.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ldm::ui {

class FindDialog;

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
    void ShowTrayMenu(POINT screen);

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
    void ShowOptions();
    void AskForLogin(const core::DownloadFailed& failed);
    void ChooseListFont();
    void ModifySettings(std::function<void(model::Settings&)> modify);
    void SetSort(SortKey key);
    void Post(core::Command command);
    [[nodiscard]] std::vector<const core::DownloadView*> Selection() const;
    [[nodiscard]] const core::DownloadView* SingleSelection() const;
    [[nodiscard]] std::shared_ptr<const core::Snapshot> Snapshot() const;
    void ShowError(const std::wstring& message) const;

    Context context_;
    TransferWindows transfers_;
    AddFlow adds_;
    std::unique_ptr<FindDialog> find_;
    FindOptions findOptions_;
    std::optional<model::CategoryId> categoryMenuTarget_;
    std::vector<HWND> trayWindows_;  // download windows listed in the open tray menu
};

}  // namespace ldm::ui
