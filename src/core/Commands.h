#pragma once

#include "model/Category.h"
#include "model/ConnectionRules.h"
#include "model/Download.h"
#include "model/Ids.h"
#include "model/Settings.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// Requests to the coordinator. The UI, the command line and IPC all speak these; every field is
// validated by the coordinator, never trusted.
namespace ldm::core {

enum class AddMode {
    StartNow,
    Later,        // keep in the list without starting ("Download Later")
    Speculative,  // start at once while the user still edits the info dialog (checklist A-03)
};

// What to do when the target file already exists on disk (checklist A-06).
enum class ExistingFilePolicy {
    AutoRename,
    Replace,
    Cancel,
};

struct AddDownloadCommand {
    std::uint64_t token = 0;  // echoed in AddResult so the caller can match the answer
    std::string url;
    std::optional<std::string> fileName;  // a name the user chose; otherwise the server's or the URL's
    std::optional<std::string> saveDir;   // otherwise the category folder
    std::optional<model::CategoryId> categoryId;
    std::string description;
    std::optional<std::string> referer;
    std::optional<std::string> parentPage;
    std::optional<std::string> loginUser;
    std::optional<std::string> loginPassword;
    AddMode mode = AddMode::StartNow;
    // Set after the user answered the duplicate prompt; otherwise Options > Downloads applies.
    std::optional<model::DuplicatePolicy> duplicatePolicy;
    ExistingFilePolicy existingFilePolicy = ExistingFilePolicy::AutoRename;
};

// The user pressed Start Download or Download Later in the info dialog of a speculative add.
struct ConfirmDownloadCommand {
    model::DownloadId id;
    std::string fileName;
    std::string saveDir;
    std::optional<model::CategoryId> categoryId;
    std::string description;
    bool start = true;
    bool rememberFolderForCategory = false;
};

struct ResumeCommand {
    std::vector<model::DownloadId> ids;
};
struct StopCommand {
    std::vector<model::DownloadId> ids;
};
struct StopAllCommand {};
struct DeleteCommand {
    std::vector<model::DownloadId> ids;
    bool deleteFiles = false;  // also delete completed files from disk (checklist L-07)
};
struct DeleteCompletedCommand {};
struct RedownloadCommand {
    model::DownloadId id;
};
struct SetDownloadSpeedLimitCommand {
    model::DownloadId id;
    std::optional<std::int64_t> bytesPerSecond;
};
// Downloads > Speed Limiter (checklist Q-09). `kilobytesPerSecond` replaces the stored value when set.
struct SetSpeedLimiterCommand {
    bool enabled = false;
    std::optional<int> kilobytesPerSecond;
};

// Properties dialog (checklist C-02). Unset fields stay as they are.
struct UpdateDownloadCommand {
    model::DownloadId id;
    std::optional<std::string> url;
    std::optional<std::string> description;
    std::optional<std::string> referer;
    std::optional<std::string> loginUser;
    std::optional<std::string> loginPassword;  // empty string clears the stored password
    std::optional<model::CompletionAction> completionAction;
    std::optional<int> maxConnections;
};

// Move/Rename (checklist L-05). For completed downloads the file on disk is moved by the caller's
// platform layer first; this records the new location.
struct MoveDownloadCommand {
    model::DownloadId id;
    std::string saveDir;
    std::string fileName;
};

struct SaveCategoryCommand {
    model::Category category;  // id 0 adds a new category
};
struct DeleteCategoryCommand {
    model::CategoryId id;
};
// Puts every download whose file type is in the category's list into that category (checklist
// W-03, offered after the list changed). Folders of existing downloads stay as they are.
struct ReassignCategoryCommand {
    model::CategoryId id;
};
struct UpdateSettingsCommand {
    model::Settings settings;
};
// Changes some settings starting from the current values on the core thread, so two front ends
// that change different settings never overwrite each other's change. In-process callers only.
struct ModifySettingsCommand {
    std::function<void(model::Settings&)> modify;
};

struct SiteLoginInput {
    std::string pattern;
    std::string username;
    std::string password;  // plain text from the dialog; protected before it is stored
};
struct ReplaceSiteLoginsCommand {
    std::vector<SiteLoginInput> logins;
};
struct ReplaceServerLimitsCommand {
    std::vector<model::ServerLimit> limits;
};

using Command = std::variant<AddDownloadCommand, ConfirmDownloadCommand, ResumeCommand, StopCommand, StopAllCommand,
                             DeleteCommand, DeleteCompletedCommand, RedownloadCommand, SetDownloadSpeedLimitCommand,
                             SetSpeedLimiterCommand, UpdateDownloadCommand, MoveDownloadCommand, SaveCategoryCommand,
                             DeleteCategoryCommand, ReassignCategoryCommand, UpdateSettingsCommand,
                             ModifySettingsCommand,
                             ReplaceSiteLoginsCommand, ReplaceServerLimitsCommand>;

// Where front ends send commands. Thread-safe; returns at once.
class CommandSink {
public:
    virtual ~CommandSink() = default;
    virtual void Post(Command command) = 0;
};

}  // namespace ldm::core
