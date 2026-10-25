#pragma once

#include "core/TransferEngine.h"
#include "model/Category.h"
#include "model/ConnectionRules.h"
#include "model/Download.h"
#include "model/Queue.h"
#include "model/Settings.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// What the coordinator publishes. Snapshots are immutable and shared: the UI keeps the newest
// one and never sees state change under it (design section 3).
namespace ldm::core {

// One row of the download list, with the live values the database does not hold.
struct DownloadView {
    model::Download download;  // secrets removed: loginSecret, cookieSecret and postSecret are always empty here
    bool hasPassword = false;
    bool awaitingConfirmation = false;  // speculative add whose info dialog is still open
    std::int64_t bytesPerSecond = 0;
    std::optional<std::int64_t> secondsLeft;
    std::int64_t receivedBytes = 0;  // includes bytes not yet flushed, for display
    std::vector<ConnectionView> connections;
    std::optional<TransferFailure> retrying;  // set while the engine retries after a failure
    bool waitingForQuota = false;  // paused by the download quota; starts again by itself
    bool waitingForAddress = false;  // Refresh download address: waits for the browser (ENG-08)
    bool waitingInQueue = false;  // a member of a running queue that waits for a free slot
    // A stream's progress, which has no total size until its pieces are joined. Its tracks are in
    // `download.tracks` without their pieces, which can be many thousands.
    struct Stream {
        int piecesDone = 0;
        int piecesTotal = 0;  // 0 while a track's list is not read yet
        double fraction = 0;  // 0..1; of what is listed so far for a live recording
        std::optional<std::int64_t> estimatedBytes;  // of what is downloaded, before joining
        bool live = false;  // a live recording: no total, the length grows while it runs
        std::int64_t recordedUs = 0;  // the length on disk
    };
    std::optional<Stream> stream;
};

struct QueueView {
    model::Queue queue;
    bool running = false;
    int members = 0;
    int runningMembers = 0;  // members that download, are being moved into place or wait for the quota
};

// A stored site login without its password.
struct SiteLoginView {
    std::string pattern;
    std::string username;
};

struct Snapshot {
    std::uint64_t version = 0;
    // Ordered by sort order. Rows that did not change are shared with the previous snapshot.
    std::vector<std::shared_ptr<const DownloadView>> downloads;
    std::vector<model::Category> categories;
    std::vector<QueueView> queues;  // in their sort order
    model::Settings settings;  // secrets inside proxy settings are protected blobs, never plain text
    std::vector<model::ServerLimit> serverLimits;
    std::vector<SiteLoginView> siteLogins;
    bool speedLimiterOn = false;
    std::int64_t totalBytesPerSecond = 0;
    int activeDownloads = 0;
    std::int64_t quotaLimitBytes = 0;  // 0: no download quota
    std::int64_t quotaUsedBytes = 0;   // in the current quota period
    std::optional<std::int64_t> quotaEndsAtUnixMs;  // while downloads wait for the quota
};

// Answer to AddDownloadCommand.
struct AddResult {
    std::uint64_t token = 0;
    std::optional<model::DownloadId> id;  // the new download, or the existing one it resolved to
    std::optional<std::string> error;     // invalid address, cancelled because the file exists, ...
    // Set when the duplicate policy is Ask and the address is already in the list: nothing was
    // added; the caller asks the user and sends the command again with a policy.
    std::optional<model::DownloadId> duplicateOf;
    bool showExistingComplete = false;  // policy "resume or show complete" hit a completed download
};

struct DownloadCompleted {
    model::DownloadId id;
    std::string fileName;
    std::string finalPath;  // UTF-8
    std::string url;
    std::optional<std::int64_t> size;
    model::CompletionAction completionAction = model::CompletionAction::None;
    // False for a Site Grabber download not added to the main list: the Grabber's own to report.
    bool inMainList = true;
};

struct DownloadFailed {
    model::DownloadId id;
    std::string fileName;
    TransferFailure failure;
    bool inMainList = true;  // as in DownloadCompleted
};

// Something the user should know that is not tied to a download (database or settings errors).
struct CoreError {
    std::string message;
};

// The download quota is used up while downloads run and Options ask to warn first (checklist
// O-05). The downloads keep running until a QuotaDecisionCommand answers.
struct QuotaReached {
    int megabytes = 0;
    int hours = 0;
    std::int64_t periodEndsAtUnixMs = 0;
};

// A download queue started, by the user or by its schedule (checklist O-09: its sound).
struct QueueStarted {
    model::QueueId id;
    std::string name;
};

// A queue stopped by itself because no member is left to download (checklist Q-08). The front end
// runs its completion action, which is None when the queue started nothing in this run, so an
// empty queue never shuts the computer down.
struct QueueFinished {
    model::QueueId id;
    std::string name;
    model::CompletionAction completionAction = model::CompletionAction::None;
    bool forceTerminate = false;
    std::string openFileWhenDone;
};

// A queue was stopped: by the user, Stop All, its schedule's stop time or its deletion. No
// completion action follows.
struct QueueStopped {
    model::QueueId id;
    std::string name;
};

// The end of an update check (checklist X-05): sent after every check the user asked for, and after
// an automatic one only when it found a newer version the user did not skip.
struct UpdateCheckFinished {
    bool manual = false;
    std::optional<std::string> newerVersion;  // "5.1.0"
    std::string pageUrl;                      // the release's page on GitHub, with newerVersion
    std::optional<std::string> error;         // the check got no usable answer
};

// FetchFinished answers a FetchManifestCommand.
using Notification = std::variant<AddResult, DownloadCompleted, DownloadFailed, CoreError, QuotaReached, FetchFinished,
                                  QueueStarted, QueueFinished, QueueStopped, UpdateCheckFinished, SiteLookupFinished>;

// Receives the coordinator's output. Called on the core thread; implementations must only post to
// their own thread.
class CoreObserver {
public:
    virtual ~CoreObserver() = default;
    virtual void OnSnapshot(std::shared_ptr<const Snapshot> snapshot) = 0;
    virtual void OnNotification(Notification notification) = 0;
};

}  // namespace ldm::core
