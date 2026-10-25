#pragma once

#include "base/Result.h"
#include "core/Commands.h"
#include "core/ManifestFetches.h"
#include "core/MediaProgress.h"
#include "core/MediaTools.h"
#include "core/Ports.h"
#include "core/QueueRunner.h"
#include "core/Quota.h"
#include "core/SiteLookups.h"
#include "core/SiteResolver.h"
#include "core/ToolInstaller.h"
#include "core/ToolUpdates.h"
#include "core/Snapshot.h"
#include "core/Store.h"
#include "core/TransferEngine.h"
#include "core/UpdateCheck.h"
#include "core/Url.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
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
    // Joins finished streams; without it a stream fails when its pieces are all on disk.
    MediaTools* media = nullptr;
    // Local time for queue schedules; without it schedules never start or stop a queue.
    TimeZone* timeZone = nullptr;
    // Wakes the computer for a scheduled start; optional.
    WakeTimer* wakeTimer = nullptr;
    // Looks up video pages of supported sites; without it they cannot be downloaded.
    SiteResolver* sites = nullptr;
    // Installs and updates the programs `sites` runs; without it they are never installed.
    ToolInstaller* tools = nullptr;
};

struct CoordinatorConfig {
    std::filesystem::path downloadsFolder;  // root for the built-in category folders on first run
    std::chrono::milliseconds checkpointInterval{2000};
    std::chrono::milliseconds snapshotInterval{100};  // at most 10 snapshots per second
    int maxAttemptsPerSegment = 10;
    int maxAutomaticRestarts = 1;  // restarts from zero after inconsistent content or a lost partial file
    // The update check (checklist X-05): the releases feed to ask and the running version. An
    // empty feed turns the check off.
    std::string updateFeedUrl;
    std::string appVersion;
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

        // Streams (M3 design section 6).
        std::optional<std::int64_t> approximateBytes;  // from the media panel, until progress tells
        std::int64_t runStartBytes = 0;   // on disk when the run started; the run reports its own
        int pendingPlaylists = 0;         // HLS playlists of this run still being fetched
        std::optional<std::string> runProxy;  // the run's proxy, kept for the engine start after them
        std::optional<TransferFailure> playlistFailure;
        std::vector<MediaPieceDone> unsavedPieces;  // done since the last checkpoint
        bool muxing = false;              // a Mux for download.epoch has not answered
        bool describing = false;          // a Describe for download.epoch has not answered
        std::optional<std::filesystem::path> muxOutput;  // the joined file, until it is moved

        // Live recordings: the playlists are read again while the run lasts.
        bool listedThisRun = false;       // the run read the playlists itself, so its pieces are fresh
        std::optional<std::chrono::steady_clock::time_point> nextPlaylistReload;
        int pendingReloads = 0;           // playlists of the current reload still being fetched
        bool reloadChanged = false;       // the current reload added something
        std::optional<TransferFailure> reloadFailure;  // of the current reload
        int reloadFailures = 0;           // reloads in a row that failed
        std::optional<TransferFailure> recordingFailure;  // the run stops, then the download fails
        bool finishRecording = false;     // join what is on disk once the run stopped

        // Site videos (video-sites design, section 8): fresh addresses asked for before the engine
        // starts, at most once per run, and runs started again after an expired address.
        bool pendingRefresh = false;
        bool refreshedThisRun = false;
        bool forceRefresh = false;        // the next run asks, whenever the addresses expire
        int expiredRestarts = 0;          // since the user last started it or it completed
        // HLS tracks whose playlists are read again after fresh addresses, as they were before:
        // what they had on disk is kept when the new list has the same pieces (KeepProgress).
        std::map<std::uint32_t, model::MediaTrack> relistedFrom;

        // Synchronization (checklist Q-05): what the completed download was before its check, put
        // back when the server's file did not change or nothing of a new one arrived.
        struct SyncCheck {
            std::vector<model::Segment> segments;
            std::optional<std::int64_t> totalSize;
            std::optional<bool> resumable;
            model::Validators validators;
            std::optional<std::string> finalUrl;
            std::optional<std::int64_t> completedAtMs;
        };
        std::optional<SyncCheck> syncCheck;

        std::shared_ptr<const DownloadView> view;  // rebuilt when `viewDirty`
        bool viewDirty = true;
    };

    // Command handlers.
    void Add(const AddDownloadCommand& command);
    void Confirm(const ConfirmDownloadCommand& command);
    void Resume(const std::vector<model::DownloadId>& ids);
    void Stop(const std::vector<model::DownloadId>& ids);
    void StopAll();
    void FinishRecording(const std::vector<model::DownloadId>& ids);
    void Delete(const std::vector<model::DownloadId>& ids, bool deleteFiles);
    void DeleteCompleted();
    void Redownload(model::DownloadId id);
    // Shows Site Grabber downloads in the main list.
    void SetListed(const std::vector<model::DownloadId>& ids);
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

    // Download queues (M4 design section 3; CoordinatorQueues.cpp).
    void SaveQueue(model::Queue queue, const std::vector<model::DownloadId>& members);
    void DeleteQueue(model::QueueId id);
    void SetQueue(const std::vector<model::DownloadId>& ids, std::optional<model::QueueId> queue);
    void ReorderQueue(model::QueueId id, const std::vector<model::DownloadId>& order);
    void StartQueue(model::QueueId id);
    // Stops a running queue and its running members; tells the front end unless `quietly`.
    void StopQueue(model::QueueId id, bool quietly = false);
    // Starts members of running queues in free slots and stops queues that are done. Runs on
    // every Tick and after a queue starts.
    void RunQueues();
    // Schedules (M4 design section 4): queues set to start with LDM, and starts and stops due now.
    void StartQueuesOnAppStart();
    void CheckSchedules();
    // The next start or stop of any schedule after the last check, and the wake timer.
    void PlanSchedules();
    [[nodiscard]] model::Queue* FindQueue(model::QueueId id);
    [[nodiscard]] std::optional<model::QueueId> MainQueue() const;
    [[nodiscard]] std::int64_t NextQueuePosition(model::QueueId id) const;
    [[nodiscard]] QueueMember DescribeMember(const Entry& entry) const;
    [[nodiscard]] bool IsWaitingInQueue(const Entry& entry) const;
    // Marks the views of a queue's members changed: whether they wait in it depends on its run.
    void MarkMembersChanged(model::QueueId id);
    // Membership consequences of a member's end: a completed member leaves an ordinary queue; a
    // failed member of a running queue counts the failure and moves behind the others.
    void LeaveQueueOnCompletion(Entry& entry);
    // Records that the running queue of `entry` finished some work (see QueueRun::completedAny).
    void NoteQueueWorkDone(const Entry& entry);
    void CountQueueFailure(Entry& entry);
    // Downloads a completed member of the synchronization queue again only when the server's file
    // changed: a conditional first request (M4 design section 5).
    void StartSyncCheck(Entry& entry);
    // The end of a check's run. Returns true when it put the completed download back: the file did
    // not change, or the check ended before anything of a new file arrived.
    [[nodiscard]] bool EndSyncCheck(Entry& entry, TransferOutcome outcome, const std::optional<TransferFailure>& failure);

    // Engine event handlers.
    void OnResponseInfo(Entry& entry, const ResponseInfo& info);
    void OnProgress(Entry& entry, const ProgressInfo& progress);
    void OnFinished(Entry& entry, const TransferFinished& finished);
    void OnFinalized(Entry& entry, const FinalizeFinished& finalized);
    void OnProxyResolved(Entry& entry, const ProxyResolved& resolved);
    void OnMediaProgress(Entry& entry, const MediaProgress& progress);
    void OnMediaFinished(Entry& entry, const MediaFinished& finished);
    void OnMuxFinished(Entry& entry, const MuxFinished& finished);
    void OnDescribeFinished(Entry& entry, const DescribeFinished& finished);
    void OnPlaylistFetched(const FetchFinished& fetched);
    void OnPlaylistReloaded(Entry& entry, std::uint32_t track, const FetchFinished& fetched);
    // What follows the end of any run, file or stream, once its own results are applied.
    void OnRunEnded(Entry& entry, TransferOutcome outcome, const std::optional<TransferFailure>& failure);

    // Lifecycle helpers.
    void Start(Entry& entry);
    void StartEngine(Entry& entry, std::optional<std::string> proxy);
    // A stream's run: its HLS playlists first when they are not read yet, then the engine.
    void StartStream(Entry& entry);
    [[nodiscard]] std::vector<std::string> StreamCookieLines(const model::Download& download);
    // A site video's other headers, as stored with it (model::Download::siteHeaderSecret).
    [[nodiscard]] std::vector<std::string> StreamSiteHeaders(const model::Download& download);
    // Them protected for storing: nothing for none; only lines media::IsSiteHeaderLine takes.
    [[nodiscard]] base::Result<std::vector<std::uint8_t>> ProtectSiteHeaders(const std::vector<std::string>& lines);
    // A live recording's playlists, read again (RFC 8216 section 6.3.4).
    void ReloadPlaylists(Entry& entry);
    // Ends a live recording that is not running: joins what is on disk, if anything.
    void FinishRecordingNow(Entry& entry);
    // A finished run's next step: the final move, or joining a stream's pieces first.
    void CompleteRun(Entry& entry);
    void BeginMux(Entry& entry);
    // Describes a finished video or audio file with the media tools before it is moved (C-04).
    void BeginDescribe(Entry& entry);
    // Stops the join or description of `entry`, if one runs.
    void CancelMediaTools(Entry& entry);
    // Stores tracks whose lists or progress changed as a whole.
    void StoreTracks(Entry& entry, const std::vector<std::uint32_t>& tracks);
    [[nodiscard]] std::optional<std::string> ValidateMedia(const MediaChoice& media, model::Download& download);
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
    [[nodiscard]] int ConnectionsFor(const Entry& entry, const std::optional<Url>& url, bool post) const;
    [[nodiscard]] base::Status SeedCategories();
    void RecoverAfterRestart(Entry& entry);

    CoordinatorPorts ports_;
    CoordinatorConfig config_;

    model::Settings settings_;
    std::vector<model::Category> categories_;
    std::vector<model::SiteLogin> siteLogins_;
    std::vector<model::ServerLimit> serverLimits_;
    std::map<model::DownloadId, Entry> entries_;
    std::vector<model::Queue> queues_;
    // The queues that run, with what their current run remembers. Not stored: every queue is
    // stopped when LDM starts.
    struct QueueRun {
        std::set<model::DownloadId> stoppedByUser;  // not started again in this run
        // Synchronization queue: members started in this run, so each is checked once per run.
        std::set<model::DownloadId> started;
        bool startedAny = false;
        // A member completed (or, in the synchronization queue, was found unchanged) in this run.
        // The completion action runs only then, and only when the user stopped no member that is
        // still unfinished: a queue the user halted by hand never shuts the computer down.
        bool completedAny = false;
    };
    std::map<model::QueueId, QueueRun> queueRuns_;
    // Schedules: starts and stops after the last check are due; the next of them, if any.
    std::int64_t lastScheduleCheckUnixMs_ = 0;
    std::optional<std::int64_t> nextScheduleEventUnixMs_;
    bool anyScheduleEnabled_ = false;
    std::optional<std::int64_t> armedWakeUnixMs_;
    std::int64_t nextSortOrder_ = 1;
    bool speedLimiterOn_ = false;
    bool shuttingDown_ = false;
    bool powerActive_ = false;

    QuotaPeriod quota_;
    bool quotaQuestionOpen_ = false;  // QuotaReached was sent and not answered yet
    std::optional<std::chrono::steady_clock::time_point> quotaIgnoredUntil_;

    // Manifest fetches for the media panel; set when the ports have an engine, a proxy resolver
    // and an observer.
    std::unique_ptr<ManifestFetches> manifests_;
    // The update check; set when there is a feed to ask as well.
    std::unique_ptr<UpdateCheck> updates_;
    // Lookups of site video pages; set when the ports have a site resolver.
    std::unique_ptr<SiteLookups> sites_;
    // yt-dlp and Deno updates; set when the ports have a tool installer, an engine and a proxy.
    std::unique_ptr<ToolUpdates> tools_;
    // Stores when `tool` was last checked, and publishes what changed.
    void OnToolChecked(SiteTool tool);
    // The site lookup's answer for a site video's run that waits for fresh addresses.
    void OnSiteRefreshed(model::DownloadId id, std::int64_t epoch, const SiteLookupFinished& answer);
    // A site video added from a list, at its first start: its tracks from `video` by its
    // siteQuality. The reason when none fit.
    [[nodiscard]] std::optional<std::string> ChooseSiteTracks(Entry& entry, const media::SiteVideo& video);
    // A site video's cookies for its resolver, as SiteLookupRequest::cookies: empty unless Options
    // let the browser sign in on its site.
    [[nodiscard]] std::string SignInCookies(const model::Download& download);
    void OnUpdateChecked(const UpdateCheckOutcome& outcome);

    // HLS playlists fetched at a stream's start or a live recording's reload, by fetch id. Their
    // ids are negative, apart from the media panel's, which are at least 1.
    enum class PlaylistPurpose {
        Initial,  // the track's list, before the run
        Reload,   // a live recording's list, read again while it runs
    };
    struct PlaylistFetch {
        model::DownloadId id;
        std::int64_t epoch = 0;
        std::uint32_t track = 0;
        PlaylistPurpose purpose = PlaylistPurpose::Initial;
    };
    std::map<std::int64_t, PlaylistFetch> playlistFetches_;
    std::int64_t lastPlaylistFetchId_ = 0;
    void FetchPlaylist(Entry& entry, std::uint32_t track, PlaylistPurpose purpose);

    // Refresh download address: the one download waiting for a new address, and since when.
    std::optional<model::DownloadId> waitingForAddress_;
    std::chrono::steady_clock::time_point waitingSince_{};

    bool snapshotDirty_ = true;
    std::uint64_t snapshotVersion_ = 0;
    std::chrono::steady_clock::time_point lastSnapshot_{};
};

}  // namespace ldm::core
