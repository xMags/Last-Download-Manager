#pragma once

#include "core/Commands.h"
#include "core/MediaProgress.h"
#include "media/SiteVideo.h"
#include "model/Download.h"
#include "model/Ids.h"
#include "model/Media.h"

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
    // Refuse connections to loopback and private addresses (core::IsPrivateIpv4): a stream from a
    // public host. Not checked through a proxy, which connects on the user's behalf.
    bool refusePrivateAddresses = false;
    // Synchronization (checklist Q-05): a fresh run that downloads only when the file on the server
    // differs from the one described by `validators` and `knownSize`. The first request asks with
    // If-None-Match or If-Modified-Since; a 304 answer, or without validators the same size, ends
    // the run as TransferOutcome::Unchanged with nothing written to the partial file.
    bool skipIfUnchanged = false;
};

// One piece of a stream's track to fetch into its own file (M3 design section 6).
struct MediaPieceRequest {
    std::uint32_t index = 0;  // in the track, as reported back in MediaPieceDone
    std::string url;
    std::optional<std::int64_t> rangeOffset;  // with rangeLength: only these bytes of `url`
    std::optional<std::int64_t> rangeLength;
    std::optional<std::uint32_t> key;  // index into MediaTrackRequest::keys; AES-128 with `iv`
    model::AesKey iv{};
    std::filesystem::path path;  // absolute; created, or replaced when a run left part of it
};

struct MediaTrackRequest {
    std::uint32_t track = 0;
    std::vector<model::MediaKey> keys;
    // The pieces not on disk yet, in playback order.
    std::vector<MediaPieceRequest> pieces;
    // A track that is one file instead: downloaded like a file into `path`, continuing `ranges`.
    bool wholeFile = false;
    std::string url;
    std::filesystem::path path;
    std::vector<model::Segment> ranges;
    // A live recording: the run does not complete when these pieces are done, but waits for
    // more through TransferEngine::ExtendMedia until an extension turns `live` off.
    bool live = false;
};

// What a live recording's playlist added since the run started or was last extended.
struct MediaTrackExtension {
    std::uint32_t track = 0;  // as in MediaTrackRequest::track
    // Keys after those the run already has; MediaPieceRequest::key indexes the whole list.
    std::vector<model::MediaKey> keys;
    std::vector<MediaPieceRequest> pieces;  // in playback order, all after the run's pieces
    bool live = true;  // false: the stream ended or the user finished the recording
};

struct MediaExtension {
    std::vector<MediaTrackExtension> tracks;
};

// Everything the engine needs to run one stream download: the pieces of every track, fetched
// several at once into separate files, decrypted as they arrive.
struct MediaRequest {
    model::DownloadId id;
    std::int64_t epoch = 0;

    std::optional<std::string> referer;
    std::string userAgent;
    std::vector<std::string> cookieLines;  // Netscape format, as in TransferRequest
    std::optional<std::string> proxy;
    bool refusePrivateAddresses = false;  // as in TransferRequest; for pieces and keys

    std::vector<MediaTrackRequest> tracks;

    int maxConnections = 8;
    std::optional<std::int64_t> speedLimitBytesPerSecond;
    int maxAttemptsPerSegment = 10;  // per piece
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
    PrivateAddress,     // a stream from a public host pointed at a loopback or private address
    UnsupportedStream,  // a live or protected stream, which LDM does not download
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
    Unchanged,  // TransferRequest::skipIfUnchanged: the server's file is the known one; the empty partial file is closed
};

struct TransferFinished {
    TransferOutcome outcome = TransferOutcome::Failed;
    std::optional<TransferFailure> failure;
    std::vector<model::Segment> segments;  // final durable progress
    std::optional<std::int64_t> totalSize;
};

// Durable byte ranges of a stream's whole-file track.
struct MediaTrackRanges {
    std::uint32_t track = 0;
    std::vector<model::Segment> ranges;
    std::optional<std::int64_t> totalSize;
};

struct MediaProgress {
    std::vector<MediaPieceDone> done;  // pieces durable since the previous report of the run
    std::vector<MediaTrackRanges> ranges;
    std::int64_t receivedBytes = 0;  // in this run, durable or not; for display
    std::int64_t bytesPerSecond = 0;
    int connections = 0;
    std::optional<TransferFailure> lastRetry;
};

// The end of a stream run: like TransferFinished, after every file of the run is closed.
struct MediaFinished {
    TransferOutcome outcome = TransferOutcome::Failed;
    std::optional<TransferFailure> failure;
    std::vector<MediaPieceDone> done;  // since the last MediaProgress
    std::vector<MediaTrackRanges> ranges;
};

struct FinalizeRequest {
    model::DownloadId id;
    std::int64_t epoch = 0;
    std::filesystem::path partialPath;
    std::filesystem::path finalPath;  // must not exist; the engine never overwrites
    std::optional<std::string> lastModified;  // set as the file time when present (checklist ENG-16)
    // A stream's parts folder, removed with everything in it once the move succeeded.
    std::optional<std::filesystem::path> removeFolder;
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

// The media tools' answer to a MuxRequest (core/MediaTools.h). Sent through the engine listener,
// like ProxyResolved, so it is ordered with engine events and dropped by its epoch when stale.
struct MuxFinished {
    bool stopped = false;  // MediaTools::Cancel ended it
    std::optional<TransferFailure> failure;
    std::filesystem::path output;  // in the parts folder
    std::string extension;         // of `output`: "mp4", "mkv" or "vtt"
    std::int64_t bytes = 0;
    // What the file holds, for the description (checklist C-04): "1280x720, 30 fps, H.264; AAC,
    // stereo, 48 kHz; 4:05". Empty when FFmpeg could not say.
    std::string mediaInfo;
};

// The media tools' answer to a DescribeRequest, sent like MuxFinished.
struct DescribeFinished {
    bool stopped = false;  // MediaTools::Cancel ended it
    // As MuxFinished::mediaInfo: "1280x720, 30 fps, H.264; AAC, stereo, 48 kHz; 4:05". Empty when
    // ffprobe could not say, which is no failure: the file is fine without it.
    std::string mediaInfo;
};

// A small download into memory: a streaming manifest LDM reads itself (M3 design section 6).
// Not a download of the list, so it has no DownloadId; its answer carries `fetchId`.
struct FetchRequest {
    std::int64_t fetchId = 0;
    std::string url;  // http or https; redirects are followed, to http or https only
    std::optional<std::string> referer;
    std::string userAgent;
    std::vector<std::string> cookieLines;  // Netscape format, as in TransferRequest
    std::optional<std::string> proxy;
    std::size_t maxBytes = 0;  // a longer body fails the fetch
    bool refusePrivateAddresses = false;  // as in TransferRequest
    // Basic, Digest, NTLM or Negotiate credentials, sent to the address's own host only.
    std::optional<TransferCredentials> credentials;
    // A HEAD request: the answer has the type and length but no body (the Grabber's size filter).
    bool headOnly = false;
    // Media types whose body is wanted ("text/html"); empty for any. Another type ends the fetch
    // after the headers, as a success with FetchFinished::bodySkipped and no body.
    std::vector<std::string> bodyTypes;
};

// The answer to a FetchRequest: exactly one per request.
struct FetchFinished {
    std::int64_t fetchId = 0;
    std::optional<TransferFailure> failure;  // nothing: `body` is the whole 2xx response
    std::string finalUrl;  // after redirects; relative addresses in the body resolve against it
    std::optional<std::string> contentType;
    std::optional<std::int64_t> contentLength;  // as the server announced it
    std::string body;
    bool bodySkipped = false;  // FetchRequest::bodyTypes did not name the type
    // The answer came from a loopback or private address, or a host named so: what the media
    // the body lists may point at (MediaRequest::refusePrivateAddresses).
    bool fromPrivateNetwork = false;
};

// The site resolver's answer to a SiteLookupRequest (core/SiteResolver.h): exactly one per
// request. Also the notification the front end gets for its own lookups, under its own id.
struct SiteLookupFinished {
    std::int64_t lookupId = 0;
    bool stopped = false;                  // SiteResolver::Cancel ended it; nothing else is set
    std::optional<media::SiteVideo> video;  // the page's video; nothing when it failed
    std::string failure;                   // words for the user when it failed
    enum class Problem {
        None,
        Video,         // this video cannot be downloaded (gone, private, protected, live, ...)
        ToolsMissing,  // the resolver's programs are not installed
        Resolver,      // the resolver failed in a way that a newer version may fix
        Busy,          // too many lookups at once; LDM did not ask
    };
    Problem problem = Problem::None;
};

using EnginePayload = std::variant<ResponseInfo, ProgressInfo, TransferFinished, FinalizeFinished, ProxyResolved,
                                   FetchFinished, MediaProgress, MediaFinished, MuxFinished, DescribeFinished,
                                   SiteLookupFinished>;

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
    // Starts a run of a stream, under the same rules as Start; answered with MediaProgress and
    // one MediaFinished instead.
    virtual void StartMedia(MediaRequest request) = 0;
    // Adds pieces to the running stream run of a live recording, or ends its wait for more.
    // Ignored when that run already finished or is stopping, or is not a stream run.
    virtual void ExtendMedia(model::DownloadId id, std::int64_t epoch, MediaExtension extension) = 0;
    // Requests a stop. Always answered by exactly one TransferFinished (MediaFinished for a
    // stream) for that epoch, sent after the run's files are closed, unless it had already finished.
    virtual void Stop(model::DownloadId id, std::int64_t epoch) = 0;
    virtual void SetSpeedLimit(model::DownloadId id, std::int64_t epoch, std::optional<std::int64_t> bytesPerSecond) = 0;
    virtual void SetGlobalSpeedLimit(std::optional<std::int64_t> bytesPerSecond) = 0;
    // Moves a completed partial file to its final name, off the caller's thread.
    virtual void Finalize(FinalizeRequest request) = 0;
    // Fetches into memory. Answered by one FetchFinished event (its EngineEvent id is empty).
    virtual void Fetch(FetchRequest request) = 0;
};

}  // namespace ldm::core
