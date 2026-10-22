#pragma once

#include "core/Commands.h"
#include "core/Snapshot.h"
#include "ui/DownloadListView.h"
#include "ui/ListModel.h"
#include "ui/ShellActions.h"
#include "ui/Theme.h"
#include "ui/Toolbar.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace ldm::ui {

class DownloadInfoDialog;

// Turns menu, toolbar and tray commands into dialogs and core commands (design section 6). Holds
// no download state of its own: it reads the newest snapshot and posts commands.
class MainController {
public:
    struct Context {
        HWND owner = nullptr;
        core::CommandSink* commands = nullptr;
        ShellActions* shell = nullptr;
        DownloadListView* list = nullptr;
        Toolbar* toolbar = nullptr;
        const Theme* theme = nullptr;
        std::function<std::shared_ptr<const core::Snapshot>()> snapshot;
        std::function<void()> exit;
        std::function<void()> restore;
        std::function<ListFilter()> currentFilter;
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

    void ShowRowMenu(POINT screen);
    void ShowCategoryMenu(std::optional<model::CategoryId> category, POINT screen);
    void ShowTrayMenu(POINT screen);

    // Adds an address the way Add URL does, used by the command line, clipboard and drop target.
    void AddAddress(const std::string& url, bool silent);

private:
    void AddUrl(std::string initialUrl);
    void AddWithInfoDialog(const std::string& url, std::optional<std::string> user, std::optional<std::string> password,
                           std::optional<model::DuplicatePolicy> duplicatePolicy);
    // Asks about an address already in the list when Options say so (checklist A-05). Returns false
    // when nothing more should happen: the user cancelled, or the existing download was resumed.
    [[nodiscard]] bool ResolveDuplicate(const std::string& url, std::optional<model::DuplicatePolicy>& policy);
    void RemoveSelected();
    void OpenSelected();
    void DefaultAction();
    void ModifySettings(std::function<void(model::Settings&)> modify);
    void SetSort(SortKey key);
    void Post(core::Command command);
    [[nodiscard]] std::vector<const core::DownloadView*> Selection() const;
    [[nodiscard]] const core::DownloadView* SingleSelection() const;
    [[nodiscard]] std::shared_ptr<const core::Snapshot> Snapshot() const;
    void ShowError(const std::wstring& message) const;

    Context context_;
    std::uint64_t nextToken_ = 1;

    // The info dialog of a speculative add while it is open, and the download it shows.
    struct PendingAdd {
        std::uint64_t token = 0;
        std::optional<model::DownloadId> id;
        DownloadInfoDialog* dialog = nullptr;
        bool cancelled = false;
        std::optional<core::ConfirmDownloadCommand> confirmation;  // sent once the id is known
    };
    std::map<std::uint64_t, PendingAdd> pendingAdds_;
};

}  // namespace ldm::ui
