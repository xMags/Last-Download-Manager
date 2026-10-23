#pragma once

#include "core/Commands.h"
#include "core/Snapshot.h"
#include "ui/Theme.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ldm::ui {

class DownloadInfoDialog;

// One address that arrives from outside the Add URL dialog: the command line, a second launch,
// the clipboard monitor or the drop target.
struct ExternalAdd {
    std::string url;
    std::optional<std::string> saveDir;   // overrides the category folder
    std::optional<std::string> fileName;  // overrides the name taken from the address
    bool silent = false;     // no dialog at all (checklist A-10 silent mode)
    bool queueOnly = false;  // add without starting
};

// An entry of an imported list (Tasks > Import).
struct ImportedEntry {
    std::string url;
    std::optional<std::string> fileName;
    std::string description;
    std::optional<std::string> referer;
};

// Every way a download enters the list from the UI (checklist A-01 to A-08): Add URL with its
// history, the duplicate question, the Download File Info dialog with speculative start, batch
// downloads and address lists.
class AddFlow {
public:
    struct Context {
        HWND owner = nullptr;
        core::CommandSink* commands = nullptr;
        const Theme* theme = nullptr;
        std::function<std::shared_ptr<const core::Snapshot>()> snapshot;
        std::function<void(model::DownloadId)> openProgress;
        std::function<void(model::DownloadId)> select;
    };

    explicit AddFlow(Context context);
    ~AddFlow();
    AddFlow(const AddFlow&) = delete;
    AddFlow& operator=(const AddFlow&) = delete;

    void AddUrl(std::string initialUrl);
    void AddAddress(const ExternalAdd& add);
    // Adds every entry stopped, without questions: duplicates get numbered names (checklist A-11).
    void AddImported(const std::vector<ImportedEntry>& entries);
    void AddBatch();
    void AddFromClipboard();
    // Lets the user pick from `urls`, then adds the checked ones.
    void AddList(std::vector<std::string> urls, const std::wstring& title);

    void OnSnapshot(const core::Snapshot& snapshot);
    void OnAddResult(const core::AddResult& result);

private:
    struct PendingAdd {
        std::uint64_t token = 0;
        std::optional<model::DownloadId> id;
        DownloadInfoDialog* dialog = nullptr;
        bool cancelled = false;
        std::optional<core::ConfirmDownloadCommand> confirmation;  // sent once the id is known
    };

    struct InfoDialogInput {
        std::string url;
        std::optional<std::string> user;
        std::optional<std::string> password;
        std::optional<model::DuplicatePolicy> duplicatePolicy;
        std::optional<std::string> saveDir;
        std::optional<std::string> fileName;
    };

    void AddWithInfoDialog(InfoDialogInput input);
    // Asks about an address already in the list when Options say so (checklist A-05). Returns false
    // when nothing more should happen: the user cancelled, or the existing download was resumed.
    [[nodiscard]] bool ResolveDuplicate(const std::string& url, std::optional<model::DuplicatePolicy>& policy);
    void AddMany(const std::vector<std::string>& urls, bool start, std::optional<std::string> user,
                 std::optional<std::string> password);
    void Post(core::Command command);
    void ModifySettings(std::function<void(model::Settings&)> modify);
    void ShowError(const std::wstring& message) const;

    Context context_;
    std::uint64_t nextToken_ = 1;
    std::map<std::uint64_t, PendingAdd> pendingAdds_;
    std::vector<std::uint64_t> progressOnAdd_;  // adds whose progress window opens once their id is known
    std::optional<std::string> categoryToSelect_;  // a category added from the info dialog
};

}  // namespace ldm::ui
