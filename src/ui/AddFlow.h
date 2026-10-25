#pragma once

#include "browser/Protocol.h"
#include "core/Commands.h"
#include "core/Snapshot.h"
#include "media/M3uList.h"
#include "model/Cookie.h"
#include "ui/BrowserLink.h"
#include "ui/StreamChoice.h"
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
class SiteVideoDialog;

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
    std::optional<core::MediaChoice> media;  // a stream from the media panel; `url` is its manifest
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
    // A stream picked in the media panel or the Download video dialog: the Download File Info
    // dialog for it, as for a captured download the user chose.
    void AddStream(StreamCapture capture);
    // The video of a page the user named (video-sites round 2, section 2.2): the Download video
    // dialog, then AddStream with the chosen quality. Says so when site videos are off.
    void AddSiteVideo(browser::SiteRequest request);
    // The site resolver's answer for the open Download video dialog; false when it is not its.
    bool OnSiteLookedUp(const core::SiteLookupFinished& finished);
    // The videos of a page that lists several (video-sites round 2, section 2.4): the checked ones
    // are added, each picking its quality by the chosen rule when it first starts.
    void AddSitePlaylist(const media::SitePlaylist& playlist, const browser::SiteRequest& request);
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
    // The links of a downloaded playlist file (checklist A-12): the Download All Links dialog
    // with every link checked, then the checked ones added without starting, with the playlist's
    // address as their referrer.
    void AddPlaylistLinks(const std::vector<media::M3uEntry>& entries, const std::string& playlistUrl);

    void OnSnapshot(const core::Snapshot& snapshot);
    void OnAddResult(const core::AddResult& result);

private:
    struct PendingAdd {
        std::uint64_t token = 0;
        std::optional<model::DownloadId> id;
        DownloadInfoDialog* dialog = nullptr;
        bool cancelled = false;
        // The queue panel is open after the info dialog: the add's answer is kept until it closes.
        bool choosingQueue = false;
        bool failed = false;  // the add was refused; its error was shown
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
        std::optional<core::MediaChoice> media;
    };

    // Where the links in a Download All Links dialog came from, sent along with each.
    struct LinkSource {
        std::optional<std::string> referrer;
        std::optional<std::string> userAgent;  // the browser's; LDM's own when absent
        std::vector<model::Cookie> cookies;
    };
    // Runs the Download All Links dialog, calls `closed` once it is closed, remembers its options,
    // then adds the checked links without starting.
    void RunLinksDialog(AllLinksDialog& dialog, const model::Settings& settings, const LinkSource& source,
                        const std::function<void()>& closed);

    void AddWithInfoDialog(InfoDialogInput input);
    // Asks about an address already in the list when Options say so (checklist A-05). Returns false
    // when nothing more should happen: the user cancelled, or the existing download was resumed.
    // A stream counts as a duplicate only with the same tracks.
    [[nodiscard]] bool ResolveDuplicate(const std::string& url, std::optional<model::DuplicatePolicy>& policy,
                                        const core::MediaChoice* media = nullptr);
    void AddMany(const std::vector<std::string>& urls, bool start, std::optional<std::string> user,
                 std::optional<std::string> password);
    // The queue panel (checklist A-04) before downloads are added without starting, when Options
    // ask for it: `batch` for several at once (a batch or Download All Links), else Download Later.
    // Nothing means the main queue, without starting it.
    struct QueueAnswer {
        model::QueueId queue;
        bool startNow = false;
    };
    [[nodiscard]] std::optional<QueueAnswer> AskForQueue(int downloads, bool batch);
    // Starts the chosen queue after the adds before it, which the core handles first.
    void StartChosenQueue(const std::optional<QueueAnswer>& answer);
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
    // The open Download video dialog. Its lookups have ids far above the media panel's, which
    // count from 1, so an answer is never taken for the other's.
    SiteVideoDialog* siteDialog_ = nullptr;
    std::int64_t nextSiteLookupId_ = 1'000'000'000'000;
};

}  // namespace ldm::ui
