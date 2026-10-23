#pragma once

#include "base/Result.h"
#include "core/Commands.h"
#include "core/Ports.h"
#include "core/Quota.h"
#include "core/Snapshot.h"
#include "core/Store.h"
#include "core/TransferEngine.h"
#include "core/Url.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ldm::core {

// Non-owning references to everything the coordinator talks to. All must outlive it.
struct CoordinatorPorts {
    Store* store = nullptr;
    TransferEngine* engine = nullptr;
    CoreObserver* observer = nullptr;
    Clock* clock = nullptr;
    FileSystem* fileSystem = nullptr;
    SecretProtector* secrets = nullptr;
    IdGenerator* ids = nullptr;
    PowerRequests* power = nullptr;
    ProxyResolver* proxy = nullptr;
};

struct CoordinatorConfig {
    std::filesystem::path downloadsFolder;  // root for the built-in category folders on first run
    std::chrono::milliseconds checkpointInterval{2000};
    std::chrono::milliseconds snapshotInterval{100};  // at most 10 snapshots per second
    int maxAttemptsPerSegment = 10;
    int maxAutomaticRestarts = 1;  // restarts from zero after inconsistent content or a lost partial file
};

// The single writer of download state (design section 3). Runs on the core thread: every method
// is called from there, so it needs no locks. It decides and persists every state change, starts
// and stops engine runs by epoch, keeps file names unique, and publishes immutable snapshots.
class Coordinator {
public:
    Coordinator(const CoordinatorPorts& ports, CoordinatorConfig config);

    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;

    // Loads everything, seeds the built-in categories on first run and repairs state left by a
    // crash (design section 5). Publishes the first snapshot.
    [[nodiscard]] base::Status Initialize();

    void Handle(Command command);
    void OnEngineEvent(EngineEvent event);

    // Saves due checkpoints and publishes a snapshot when something changed. Call when
    // NextDeadline() passes and after handling input.
    void Tick();
    [[nodiscard]] std::chrono::steady_clock::time_point NextDeadline() const;

    // Stops every transfer (design section 3, shutdown). Further commands are ignored.
    void BeginShutdown();
    // True once no engine run or finalize is outstanding.
    [[nodiscard]] bool ShutdownComplete() const;
    // Writes the latest durable state of every download. Called last, even after a timeout.
    void FinalCheckpoint();

private:
    struct Entry {
        model::Download download;

        bool running = false;           // an engine run for download.epoch has not finished
        bool stopRequested = false;     // Stop() was sent for the current run
        bool finalizing = false;        // a Finalize for download.epoch has not answered
        bool userNamed = false;         // the user chose the name; server names do not replace it
        bool userChoseCategory = false;
        bool userChoseFolder = false;   // the save folder was given; a server name does not move it
        bool deleteWhenStopped = false;
        bool deleteFilesWhenStopped = false;
        bool restartWhenStopped = false;
        bool resumeWhenStopped = false;  // a new address arrived while the old run was stopping
        bool replaceFinalFile = false;  // an existing final file may be replaced at finalize
        bool holdFinalize = false;      // completed while the info dialog was still open
        int finalizeAttempts = 0;
        int automaticRestarts = 0;
        std::optional<int> forcedMaxConnections;

        std::chrono::steady_clock::time_point lastCheckpoint{};
        bool unsaved = false;

        std::int64_t bytesPerSecond = 0;
        std::int64_t receivedBytes = 0;
        std::vector<ConnectionView> connections;
        std::optional<TransferFailure> retrying;
        bool awaitingProxy = false;          // running, but the engine starts once the proxy is known
        bool quotaHeld = false;              // paused by the quota; started again when it allows
        std::int64_t quotaCountedBytes = 0;  // received bytes of this run already counted

        std::shared_ptr<const DownloadView> view;  // rebuilt when `viewDirty`
        bool viewDirty = true;
    };

    // Command handlers.
    void Add(const AddDownloadCommand& command);
    void Confirm(const ConfirmDownloadCommand& command);
    void Resume(const std::vector<model::DownloadId>& ids);
    void Stop(const std::vector<model::DownloadId>& ids);
    void StopAll();
    void Delete(const std::vector<model::DownloadId>& ids, bool deleteFiles);
    void DeleteCompleted();
    void Redownload(model::DownloadId id);
    void SetDownloadSpeedLimit(model::DownloadId id, std::optional<std::int64_t> bytesPerSecond);
    void SetSpeedLimiter(bool enabled, std::optional<int> kilobytesPerSecond);
    void Update(const UpdateDownloadCommand& command);
    void Move(const MoveDownloadCommand& command);
    void SaveCategory(model::Category category);
    void DeleteCategory(model::CategoryId id);
    void ReassignCategory(model::CategoryId id);
    void UpdateSettings(model::Settings settings);
    void ReplaceSiteLogins(const std::vector<SiteLoginInput>& logins);
    void ReplaceServerLimits(std::vector<model::ServerLimit> limits);
    void DecideQuota(bool keepDownloading);
    void WaitForAddress(model::DownloadId id);
    void ReplaceAddress(const ReplaceAddressCommand& command);
    // Ends Refresh download address for `id`, if it is the download that waits.
    void StopWaitingForAddress(model::DownloadId id);

    // Engine event handlers.
    void OnResponseInfo(Entry& entry, const ResponseInfo& info);
    void OnProgress(Entry& entry, const ProgressInfo& progress);
    void OnFinished(Entry& entry, const TransferFinished& finished);
    void OnFinalized(Entry& entry, const FinalizeFinished& finalized);
    void OnProxyResolved(Entry& entry, const ProxyResolved& resolved);

    // Lifecycle helpers.
    void Start(Entry& entry);
    void StartEngine(Entry& entry, std::optional<std::string> proxy);
    void RequestStop(Entry& entry);
    void ResetProgress(Entry& entry);
    void RemoveEntry(model::DownloadId id, bool deleteFinalFile);
    void BeginFinalize(Entry& entry);
    void Fail(Entry& entry, const TransferFailure& failure);

    // Names and paths.
    [[nodiscard]] std::filesystem::path FinalPath(const model::Download& download) const;
    [[nodiscard]] bool IsReserved(const std::filesystem::path& path, std::optional<model::DownloadId> except) const;
    [[nodiscard]] std::optional<std::string> UniqueFileName(const std::filesystem::path& directory,
                                                            const std::string& name,
                                                            std::optional<model::DownloadId> except) const;
    [[nodiscard]] model::CategoryId CategoryFor(const std::string& fileName, const Url& url) const;
    [[nodiscard]] const model::Category* FindCategory(model::CategoryId id) const;
    [[nodiscard]] std::string FolderFor(model::CategoryId id) const;

    // Persistence and publication.
    void Save(Entry& entry);
    void ReportError(std::string message);
    void Notify(Notification notification);
    void MarkChanged(Entry& entry);
    void PublishSnapshot();
    void UpdatePowerRequest();
    void ApplySpeedLimiter();

    // Download quota (checklist O-05).
    [[nodiscard]] std::chrono::minutes QuotaLength() const;
    [[nodiscard]] bool QuotaBlocks(std::chrono::steady_clock::time_point now) const;
    // Stops or asks when the quota is used up, and restarts held downloads once it allows.
    void EnforceQuota();
    void HoldForQuota(Entry& entry);
    void StopForQuota();
    [[nodiscard]] Entry* Find(model::DownloadId id);
    [[nodiscard]] TransferRequest BuildRequest(Entry& entry);
    [[nodiscard]] base::Status SeedCategories();
    void RecoverAfterRestart(Entry& entry);

    CoordinatorPorts ports_;
    CoordinatorConfig config_;

    model::Settings settings_;
    std::vector<model::Category> categories_;
    std::vector<model::SiteLogin> siteLogins_;
    std::vector<model::ServerLimit> serverLimits_;
    std::map<model::DownloadId, Entry> entries_;
    std::int64_t nextSortOrder_ = 1;
    bool speedLimiterOn_ = false;
    bool shuttingDown_ = false;
    bool powerActive_ = false;

    QuotaPeriod quota_;
    bool quotaQuestionOpen_ = false;  // QuotaReached was sent and not answered yet
    std::optional<std::chrono::steady_clock::time_point> quotaIgnoredUntil_;

    // Refresh download address: the one download waiting for a new address, and since when.
    std::optional<model::DownloadId> waitingForAddress_;
    std::chrono::steady_clock::time_point waitingSince_{};

    bool snapshotDirty_ = true;
    std::uint64_t snapshotVersion_ = 0;
    std::chrono::steady_clock::time_point lastSnapshot_{};
};

}  // namespace ldm::core
