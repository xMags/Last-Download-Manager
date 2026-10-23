#pragma once

#include "core/Commands.h"
#include "model/Download.h"
#include "model/Ids.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ldm::core {

// Plain-text credentials for one request. Decrypted by the core just before a start and never stored.
struct TransferCredentials {
    std::string user;
    std::string password;
};

// Everything the engine needs to run one download. The engine never reads the database or settings.
struct TransferRequest {
    model::DownloadId id;
    std::int64_t epoch = 0;

    std::string url;
    std::optional<std::string> referer;
    std::string userAgent;
    std::optional<TransferCredentials> credentials;
    // Browser cookies in Netscape format, decrypted just before the start like credentials.
    // libcurl sends each one only to its own host and path, also after redirects.
    std::vector<std::string> cookieLines;
    // A form submission to repeat (POST), decrypted just before the start. Always one connection:
    // sending a form several times at once can have effects on the site.
    std::optional<PostData> post;
    std::optional<std::string> proxy;  // libcurl proxy URL; absent = direct connection
    bool ftpActiveMode = false;        // Options > Proxy: passive FTP is the default

    std::filesystem::path partialPath;  // absolute

    // Facts from an earlier run. With progress in `segments`, the engine resumes and validates them.
    std::optional<std::int64_t> knownSize;
    std::optional<bool> resumable;
    model::Validators validators;
    std::optional<std::string> knownContentType;
    bool ignoreLastModified = false;  // Options > Downloads; a size change always counts as a change
    std::vector<model::Segment> segments;

    int maxConnections = 8;
    std::optional<std::int64_t> speedLimitBytesPerSecond;
    int maxAttemptsPerSegment = 10;
};

// Why a run stopped short. Each reason maps to one user-visible message.
enum class FailureReason {
    Dns,
    ConnectionRefused,
    ConnectionReset,
    Timeout,
    Proxy,
    Tls,
    AuthRequired,
    ProxyAuthRequired,
    NotFound,
    HttpError,
    HtmlInsteadOfFile,
    ResourceChanged,      // size or validators differ from the earlier run; bytes must not be mixed
    InconsistentContent,  // overlap bytes differ between connections (checklist ENG-03)
    ResumeUnsupported,    // progress exists but the server cannot serve ranges (checklist ENG-05)
    PartialFileMissing,   // the partial file is gone or shorter than the stored progress
    DiskFull,
    FileTooLarge,  // over the volume's file size limit (FAT32)
    DiskError,
    PermissionDenied,
    Protocol,
    Internal,
};

enum class FailureClass {
    Retryable,  // the engine already retried up to the attempt limit
    NeedsUser,
    Fatal,
};

struct TransferFailure {
    FailureReason reason = FailureReason::Internal;
    FailureClass failureClass = FailureClass::Fatal;
    int httpStatus = 0;
    std::string message;  // technical detail for Properties and logs; never contains credentials
};

// First accepted response of a run: what the server says about the file.
struct ResponseInfo {
    std::string finalUrl;
    std::optional<std::int64_t> totalSize;
    bool resumable = false;
    model::Validators validators;
    std::optional<std::string> contentType;
    std::optional<std::string> fileName;  // from Content-Disposition, decoded, not sanitized
};

// One connection, for the progress window's connection table (checklist P-03).
struct ConnectionView {
    std::int64_t segmentStart = 0;
    std::optional<std::int64_t> segmentEnd;
    std::int64_t received = 0;  // bytes of this segment received so far, durable or not
    bool waitingToRetry = false;
};

struct ProgressInfo {
    // Current view of resumability: an extra connection can reveal that ranges are not served.
    bool resumable = false;
    // Durable progress only: `done` counts bytes flushed to disk. Safe to checkpoint as is.
    std::vector<model::Segment> segments;
    std::optional<std::int64_t> totalSize;
    std::int64_t receivedBytes = 0;  // includes bytes not yet flushed; for display
    std::int64_t bytesPerSecond = 0;
    std::vector<ConnectionView> connections;
    std::optional<TransferFailure> lastRetry;  // the most recent retried failure, for "restarting" status
};

enum class TransferOutcome {
    Completed,  // every byte is flushed and the partial file is closed
    Stopped,    // acknowledged Stop(); the partial file is closed
    Failed,     // the partial file is closed
};

struct TransferFinished {
    TransferOutcome outcome = TransferOutcome::Failed;
    std::optional<TransferFailure> failure;
    std::vector<model::Segment> segments;  // final durable progress
    std::optional<std::int64_t> totalSize;
};

struct FinalizeRequest {
    model::DownloadId id;
    std::int64_t epoch = 0;
    std::filesystem::path partialPath;
    std::filesystem::path finalPath;  // must not exist; the engine never overwrites
    std::optional<std::string> lastModified;  // set as the file time when present (checklist ENG-16)
};

struct FinalizeFinished {
    std::optional<TransferFailure> failure;
};

// The proxy of a run whose lookup could not answer at once (design section 4). Sent by the proxy
// resolver, not the engine, but through the same listener, so it is ordered with engine events
// and a late answer for an older run is dropped by its epoch.
struct ProxyResolved {
    std::optional<std::string> proxy;  // nothing: connect directly
};

using EnginePayload = std::variant<ResponseInfo, ProgressInfo, TransferFinished, FinalizeFinished, ProxyResolved>;

struct EngineEvent {
    model::DownloadId id;
    std::int64_t epoch = 0;
    EnginePayload payload;
};

// Receives engine events. Called on engine threads, so implementations must only enqueue.
class EngineListener {
public:
    virtual ~EngineListener() = default;
    virtual void OnEngineEvent(EngineEvent event) = 0;
};

// Transfer engine port. All methods are thread-safe and return immediately; results arrive as
// events. Every event carries the epoch of the request it belongs to.
class TransferEngine {
public:
    virtual ~TransferEngine() = default;

    // Starts a run. A download may have at most one run at a time; the caller waits for
    // TransferFinished before starting the next epoch.
    virtual void Start(TransferRequest request) = 0;
    // Requests a stop. Always answered by exactly one TransferFinished for that epoch, sent after
    // the partial file is closed, unless the run had already finished.
    virtual void Stop(model::DownloadId id, std::int64_t epoch) = 0;
    virtual void SetSpeedLimit(model::DownloadId id, std::int64_t epoch, std::optional<std::int64_t> bytesPerSecond) = 0;
    virtual void SetGlobalSpeedLimit(std::optional<std::int64_t> bytesPerSecond) = 0;
    // Moves a completed partial file to its final name, off the caller's thread.
    virtual void Finalize(FinalizeRequest request) = 0;
};

}  // namespace ldm::core
