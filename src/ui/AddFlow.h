#pragma once

#include "browser/Protocol.h"
#include "core/Commands.h"
#include "core/Snapshot.h"
#include "model/Cookie.h"
#include "ui/BrowserLink.h"
#include "ui/Theme.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ldm::ui {

class AllLinksDialog;
class DownloadInfoDialog;

// What the browser sent along with a captured download.
struct BrowserRequest {
    std::optional<std::string> referrer;  // the page the download started from
    std::string userAgent;
    std::vector<model::Cookie> cookies;
    std::optional<core::PostData> post;  // the form submission the download answered
    std::optional<std::string> mimeType;  // the type the browser received
    bool chosen = false;  // the user picked LDM for it, so cancelling it says nothing about the capture rules
};

// One address that arrives from outside the Add URL dialog: the command line, a second launch,
// the clipboard monitor, the drop target or the browser extension.
struct ExternalAdd {
    std::string url;
    std::optional<std::string> saveDir;   // overrides the category folder
    std::optional<std::string> fileName;  // overrides the name taken from the address
    bool silent = false;     // no dialog at all (checklist A-10 silent mode)
    bool queueOnly = false;  // add without starting
    std::optional<BrowserRequest> browser;  // set for downloads captured from a browser
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
        // The Download All Links dialog of this request closed (BrowserSessions::LinksDone).
        std::function<void(BrowserConnectionId, const std::string&)> linksDone;
        // A download captured from the browser was added; its Download File Info dialog was cancelled.
        std::function<void(model::DownloadId)> capturedAdded;
        std::function<void(const std::string&)> captureCancelled;
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

    // A page's links from the browser's right-click menu (checklist B-05): the Download All Links
    // dialog, then the checked links are added without starting.
    void AddPageLinks(browser::Links links, BrowserConnectionId connection, const std::string& requestId);
    // Call when a Links request is queued, so what the browser reports about its links before the
    // dialog opens is kept for it.
    void ExpectLinks(BrowserConnectionId connection);
    void OnLinkInfo(BrowserConnectionId connection, browser::LinkInfo info);
    void OnBrowserClosed(BrowserConnectionId connection);

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
        std::optional<BrowserRequest> browser;
    };

    void AddWithInfoDialog(InfoDialogInput input);
    // Asks about an address already in the list when Options say so (checklist A-05). Returns false
    // when nothing more should happen: the user cancelled, or the existing download was resumed.
    [[nodiscard]] bool ResolveDuplicate(const std::string& url, std::optional<model::DuplicatePolicy>& policy);
    void AddMany(const std::vector<std::string>& urls, bool start, std::optional<std::string> user,
                 std::optional<std::string> password);
    void Post(core::Command command);
    // Copies the browser's referrer, User-Agent, cookies and type into an add command, and
    // remembers the command as a capture.
    void ApplyBrowserRequest(const std::optional<BrowserRequest>& browser, core::AddDownloadCommand& command);
    void ModifySettings(std::function<void(model::Settings&)> modify);
    void ShowError(const std::wstring& message) const;

    Context context_;
    std::uint64_t nextToken_ = 1;
    std::map<std::uint64_t, PendingAdd> pendingAdds_;
    std::vector<std::uint64_t> progressOnAdd_;  // adds whose progress window opens once their id is known
    std::optional<std::string> categoryToSelect_;  // a category added from the info dialog
    std::set<std::uint64_t> captureTokens_;  // adds of browser captures not answered yet

    // Link details that arrived for a queued Links request before its dialog opened.
    struct EarlyLinkInfo {
        std::vector<browser::LinkDetails> items;
        bool closed = false;  // the browser's connection ended meanwhile
    };
    std::map<BrowserConnectionId, EarlyLinkInfo> earlyLinkInfo_;
    // The open Download All Links dialog and the connection that feeds it.
    BrowserConnectionId linksConnection_ = 0;
    AllLinksDialog* linksDialog_ = nullptr;
};

}  // namespace ldm::ui
