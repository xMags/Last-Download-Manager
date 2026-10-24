#pragma once

#include "core/Commands.h"
#include "core/GrabberCommands.h"
#include "core/GrabberStore.h"
#include "core/Ports.h"
#include "core/Snapshot.h"
#include "core/TransferEngine.h"
#include "core/Url.h"
#include "grabber/GrabberFilters.h"
#include "grabber/GrabberSnapshot.h"
#include "grabber/HtmlLinks.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace ldm::grabber {

// Non-owning references to everything the explorer talks to. All must outlive it.
struct ExplorerPorts {
    core::GrabberStore* store = nullptr;
    core::TransferEngine* engine = nullptr;
    core::ProxyResolver* proxy = nullptr;
    // The coordinator's commands: the Grabber's files are ordinary downloads (decision 7).
    core::CommandSink* downloads = nullptr;
    core::FileSystem* fileSystem = nullptr;
    core::SecretProtector* secrets = nullptr;
    core::Clock* clock = nullptr;
    // Local time for project schedules; without it projects never start by themselves.
    core::TimeZone* timeZone = nullptr;
    // Wakes the computer for a project's scheduled start; optional. Not the coordinator's: each
    // arms its own timer.
    core::WakeTimer* wakeTimer = nullptr;
    GrabberObserver* observer = nullptr;
};

// The caps of M4 design section 6.1.
struct ExplorerConfig {
    std::size_t maxPageBytes = 8 * 1024 * 1024;
    int maxPagesExplored = 50'000;
    std::size_t maxItems = 200'000;
    std::size_t maxLinksPerPage = 5'000;
    int maxFileAttempts = 2;  // the reference's two attempts per file
    // Pages to save are kept until exploring ends, up to this many bytes per project.
    std::size_t maxPendingPageBytes = 256 * 1024 * 1024;
    std::chrono::milliseconds snapshotInterval{500};
    std::chrono::milliseconds saveInterval{2000};
};

// The Site Grabber (M4 design section 6): explores projects breadth first from their start pages,
// lists what it finds through the project's filters, downloads checked files as hidden downloads
// of the main list, saves pages for offline browsing and runs project schedules. Runs on the core
// thread beside the coordinator and never calls it: downloads are commands posted to it, and its
// output comes back through the coordinator's observer (CoreOutputTee).
class GrabberExplorer final : public core::GrabberComponent {
public:
    GrabberExplorer(const ExplorerPorts& ports, ExplorerConfig config);

    GrabberExplorer(const GrabberExplorer&) = delete;
    GrabberExplorer& operator=(const GrabberExplorer&) = delete;

    void Initialize() override;
    void Handle(core::GrabberCommand command) override;
    void OnEngineEvent(core::EngineEvent event) override;
    void Tick() override;
    [[nodiscard]] std::chrono::steady_clock::time_point NextDeadline() const override;
    void Shutdown() override;

    // The coordinator's output, on the core thread.
    void OnCoreSnapshot(std::shared_ptr<const core::Snapshot> snapshot);
    // Takes the coordinator's answers that belong to the Grabber: AddResult for its own adds, and
    // the end of downloads not in the main list. True when the front end should not see it.
    [[nodiscard]] bool TakeNotification(const core::Notification& notification);

    // Add tokens at or above this are the explorer's.
    static constexpr std::uint64_t kTokenBase = std::uint64_t{1} << 62;

private:
    // A page read for saving, written when exploring ends.
    struct PendingPage {
        std::int64_t itemId = 0;
        std::string body;
        std::string base;  // what its relative links resolve against
        std::vector<HtmlLink> links;
    };

    struct Item {
        model::GrabberItem data;
        std::shared_ptr<const model::GrabberItem> view;  // rebuilt when `viewDirty`
        bool viewDirty = true;
        int otherSitesDepth = 0;   // this run's; not stored
        bool readThisRun = false;  // a page read, or queued to be read, in this exploration
        bool sizeAsked = false;    // a HEAD request asked for the size in this exploration
        int failures = 0;          // download attempts that failed in this run
    };

    struct Project {
        model::GrabberProject stored;
        std::optional<std::string> damaged;
        std::optional<core::Url> start;
        BaseSite site;
        bool startIsPrivate = false;
        bool open = false;
        bool editing = false;

        bool itemsLoaded = false;
        std::vector<Item> items;  // items[i] has id i + 1
        std::unordered_map<std::string, std::int64_t> byAddress;
        std::set<std::int64_t> unsaved;

        bool exploring = false;
        // Counts explorations and stops: an answer to a fetch of an earlier one is dropped, since
        // a new exploration may have given its item id to another address.
        std::uint64_t run = 0;
        std::optional<std::string> startPageFailure;  // of the last exploration
        bool scheduledRun = false;      // its schedule started the current run
        bool downloadAfterExplore = false;
        std::deque<std::int64_t> pageQueue;
        std::deque<std::int64_t> sizeQueue;  // files whose size a filter needs
        int fetching = 0;
        int pagesExplored = 0;
        bool limitReached = false;
        std::vector<PendingPage> pendingPages;
        std::size_t pendingBytes = 0;
        std::vector<std::string> cookieLines;  // this run's browser cookies, in memory only
        std::vector<model::Cookie> cookies;

        bool downloading = false;
        std::deque<std::int64_t> downloadQueue;
        // Add token -> item. Kept after a stop until answered, so the answer can adopt or stop the
        // download it made and the item is never added twice.
        std::map<std::uint64_t, std::int64_t> adding;
        std::map<model::DownloadId, std::int64_t> active;  // download -> item
        std::set<model::DownloadId> seenRunning;  // active downloads seen running since started
    };

    struct Fetch {
        model::GrabberProjectId project;
        std::int64_t item = 0;
        bool head = false;
        std::uint64_t run = 0;  // Project::run when it was sent
    };

    // Commands.
    void SaveProject(core::GrabberSaveProjectCommand command);
    void DeleteProject(model::GrabberProjectId id);
    void SetOpen(model::GrabberProjectId id, bool open);
    void Explore(model::GrabberProjectId id, bool keepChecks, std::vector<model::Cookie> cookies, bool scheduled);
    void StopProject(Project& project);
    void Check(const core::GrabberCheckCommand& command);
    void StartDownloads(Project& project, bool scheduled);
    void StopDownloads(Project& project);
    void AddToMainList(Project& project, const std::vector<std::int64_t>& items);
    void RemoveHiddenDownloads(model::GrabberProjectId id);
    // Adds whose items are gone (a new list, a deleted project): their downloads are removed when
    // the answers come.
    void AbandonPendingAdds(Project& project);
    [[nodiscard]] static bool AddPending(const Project& project, std::int64_t itemId);

    // Exploring.
    void PumpFetches(Project& project);
    void StartFetch(Project& project, std::int64_t itemId, bool head);
    void OnFetched(const Fetch& fetch, const core::FetchFinished& finished);
    void OnPageRead(Project& project, Item& item, const core::FetchFinished& finished);
    void AddLink(Project& project, const Item& parent, const HtmlLink& link, const std::string& base);
    void ClassifyFile(Project& project, Item& item, std::optional<std::int64_t> size);
    void WritePage(Project& project, const PendingPage& page);
    void WritePendingPages(Project& project);
    void FinishExploreIfDone(Project& project);

    // Downloading.
    void PumpDownloads(Project& project);
    void StartDownload(Project& project, Item& item);
    void OnDownloadEnded(Project& project, std::int64_t itemId, bool completed, const std::string& finalPath,
                         std::optional<std::int64_t> size);
    void OnAddAnswered(Project& project, std::int64_t itemId, std::optional<model::DownloadId> downloadId);
    void FinishDownloadsIfDone(Project& project);
    [[nodiscard]] const core::DownloadView* FindDownload(model::DownloadId id) const;

    // Where things are saved (checklist G-02).
    [[nodiscard]] std::optional<std::filesystem::path> SaveRoot(const Project& project) const;
    [[nodiscard]] bool Offline(const Project& project) const;

    // Schedules (checklist G-08).
    void CheckSchedules();
    void PlanSchedules();

    // Items and state.
    [[nodiscard]] Project* Find(model::GrabberProjectId id);
    [[nodiscard]] bool LoadItems(Project& project);
    void UnloadItemsIfIdle(Project& project);
    [[nodiscard]] Item* FindItem(Project& project, std::int64_t id);
    Item& NewItem(Project& project, model::GrabberItem data, std::string key);
    void Changed(Project& project, Item& item);
    void SaveItems(Project& project);
    void SaveProjectRow(Project& project);
    void Prepare(Project& project);
    [[nodiscard]] std::optional<core::TransferCredentials> CredentialsFor(const Project& project,
                                                                          const core::Url& url) const;
    void ReportError(std::string message);
    void Publish();
    [[nodiscard]] GrabberStats StatsOf(const Project& project) const;

    ExplorerPorts ports_;
    ExplorerConfig config_;
    std::map<model::GrabberProjectId, Project> projects_;
    std::map<std::int64_t, Fetch> fetches_;                       // by fetch id
    std::map<std::int64_t, core::FetchRequest> waitingForProxy_;  // by fetch id
    std::set<std::uint64_t> abandonedAdds_;                        // add tokens, see AbandonPendingAdds
    std::int64_t nextFetchId_ = core::kGrabberFetchIdBase;
    std::uint64_t nextToken_ = kTokenBase;
    std::shared_ptr<const core::Snapshot> core_;
    bool shutDown_ = false;

    std::int64_t lastScheduleCheckUnixMs_ = 0;
    std::optional<std::int64_t> nextScheduleEventUnixMs_;
    std::optional<std::int64_t> armedWakeUnixMs_;

    bool snapshotDirty_ = true;
    std::uint64_t snapshotVersion_ = 0;
    std::chrono::steady_clock::time_point lastSnapshot_{};
    std::chrono::steady_clock::time_point lastSave_{};
};

// Hands the coordinator's output to the explorer first, then to the front end, leaving out what
// the explorer took. Called on the core thread.
class CoreOutputTee final : public core::CoreObserver {
public:
    CoreOutputTee(GrabberExplorer& explorer, core::CoreObserver& frontEnd) : explorer_(explorer), frontEnd_(frontEnd) {}

    void OnSnapshot(std::shared_ptr<const core::Snapshot> snapshot) override;
    void OnNotification(core::Notification notification) override;

private:
    GrabberExplorer& explorer_;
    core::CoreObserver& frontEnd_;
};

}  // namespace ldm::grabber
