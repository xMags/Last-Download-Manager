#pragma once

#include "model/Category.h"
#include "model/ConnectionRules.h"
#include "model/Cookie.h"
#include "model/Download.h"
#include "model/Ids.h"
#include "model/Settings.h"

#include <chrono>
#include <cstddef>
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

// The form submission a download came from (checklist ENG-11).
struct PostData {
    std::string contentType;
    std::string body;  // bytes, sent as is
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
    // From a browser capture: the browser's own User-Agent and the cookies it would send.
    std::optional<std::string> userAgent;
    std::vector<model::Cookie> cookies;
    std::optional<PostData> post;
    // The type the browser received (a capture), so a web page in the file's place is recognised
    // (checklist ENG-15). Ignored when it is itself a web page type.
    std::optional<std::string> contentType;
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

// The answer to QuotaReached when Options ask to warn before stopping (checklist O-05).
struct QuotaDecisionCommand {
    bool keepDownloading = false;  // true: ignore the quota until the current period ends
};

// How long a download waits for a new address (checklist ENG-08).
inline constexpr std::chrono::minutes kAddressWaitLimit{10};

// Refresh download address (checklist ENG-08): the download stops and waits for the browser to
// hand over a new address for the same file. One download waits at a time; a newer request takes
// over, and waiting ends after kAddressWaitLimit, on Resume, or when the download is deleted.
struct WaitForAddressCommand {
    model::DownloadId id;
};
// The new address for the waiting download, with what the browser sent along; the download
// resumes from it, with the usual checks that the file did not change. Ignored when that
// download no longer waits.
struct ReplaceAddressCommand {
    model::DownloadId id;
    std::string url;
    std::optional<std::string> referer;
    std::optional<std::string> userAgent;
    std::vector<model::Cookie> cookies;
    std::optional<PostData> post;
};

// Fetch a streaming manifest into memory for the media panel (M3 design section 6), with the
// proxy downloads use. Answered by a FetchFinished notification with the same id.
struct FetchManifestCommand {
    std::int64_t fetchId = 0;  // chosen by the caller, at least 1
    std::string url;  // http or https
    std::optional<std::string> referer;
    std::optional<std::string> userAgent;  // the browser's; Options' when absent
    std::vector<model::Cookie> cookies;
    std::size_t maxBytes = 0;
};

using Command = std::variant<AddDownloadCommand, ConfirmDownloadCommand, ResumeCommand, StopCommand, StopAllCommand,
                             DeleteCommand, DeleteCompletedCommand, RedownloadCommand, SetDownloadSpeedLimitCommand,
                             SetSpeedLimiterCommand, UpdateDownloadCommand, MoveDownloadCommand, SaveCategoryCommand,
                             DeleteCategoryCommand, ReassignCategoryCommand, UpdateSettingsCommand,
                             ModifySettingsCommand, ReplaceSiteLoginsCommand, ReplaceServerLimitsCommand,
                             QuotaDecisionCommand, WaitForAddressCommand, ReplaceAddressCommand, FetchManifestCommand>;

// Where front ends send commands. Thread-safe; returns at once.
class CommandSink {
public:
    virtual ~CommandSink() = default;
    virtual void Post(Command command) = 0;
};

}  // namespace ldm::core
