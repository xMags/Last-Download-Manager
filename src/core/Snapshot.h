#pragma once

#include "core/TransferEngine.h"
#include "model/Category.h"
#include "model/ConnectionRules.h"
#include "model/Download.h"
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
    // A stream's progress, which has no total size until its pieces are joined. Its tracks are in
    // `download.tracks` without their pieces, which can be many thousands.
    struct Stream {
        int piecesDone = 0;
        int piecesTotal = 0;  // 0 while a track's list is not read yet
        double fraction = 0;  // 0..1
        std::optional<std::int64_t> estimatedBytes;  // of what is downloaded, before joining
    };
    std::optional<Stream> stream;
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
};

struct DownloadFailed {
    model::DownloadId id;
    std::string fileName;
    TransferFailure failure;
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

// FetchFinished answers a FetchManifestCommand.
using Notification = std::variant<AddResult, DownloadCompleted, DownloadFailed, CoreError, QuotaReached, FetchFinished>;

// Receives the coordinator's output. Called on the core thread; implementations must only post to
// their own thread.
class CoreObserver {
public:
    virtual ~CoreObserver() = default;
    virtual void OnSnapshot(std::shared_ptr<const Snapshot> snapshot) = 0;
    virtual void OnNotification(Notification notification) = 0;
};

}  // namespace ldm::core
