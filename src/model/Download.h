#pragma once

#include "model/Ids.h"
#include "model/Media.h"
#include "model/Segment.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ldm::model {

// Lifecycle of a download. Transitions are decided only by the core coordinator.
//   Queued -> Downloading -> Finalizing -> Completed
//   Downloading -> Stopping -> Paused       (user pause, acknowledged by the engine)
//   Downloading -> Failed                   (non-retryable error or retries exhausted)
//   any -> Deleting -> (row removed)        (waits for the engine to release files)
enum class DownloadStatus {
    Queued,
    Downloading,
    Stopping,
    Paused,
    Finalizing,
    Completed,
    Failed,
    Deleting,
};

[[nodiscard]] std::string_view ToString(DownloadStatus status) noexcept;
[[nodiscard]] std::optional<DownloadStatus> ParseDownloadStatus(std::string_view text) noexcept;

// Action run when a single download completes (checklist P-05).
enum class CompletionAction {
    None,
    ExitApp,
    Shutdown,
    Sleep,
    Hibernate,
};

[[nodiscard]] std::string_view ToString(CompletionAction action) noexcept;
[[nodiscard]] std::optional<CompletionAction> ParseCompletionAction(std::string_view text) noexcept;

// Server-provided identity of the remote file, used to refuse resuming a changed resource.
struct Validators {
    std::optional<std::string> etag;
    std::optional<std::string> lastModified;

    auto operator<=>(const Validators&) const = default;
};

struct Download {
    DownloadId id;
    std::string publicId;  // random UUID; the only identifier exposed outside the process

    std::string url;
    std::optional<std::string> finalUrl;
    std::optional<std::string> referer;
    std::optional<std::string> parentPage;
    std::optional<std::string> userAgent;

    std::string fileName;  // UTF-8, already sanitized
    std::string saveDir;   // UTF-8 absolute path
    std::optional<std::string> partialPath;
    std::optional<CategoryId> categoryId;
    std::string description;

    DownloadStatus status = DownloadStatus::Queued;
    std::int64_t epoch = 0;
    // A speculative add whose Download File Info dialog has not been answered (checklist A-03).
    // Stored so that a row left by a crash while the dialog was open is removed at the next start.
    bool awaitingConfirmation = false;

    std::optional<std::int64_t> totalSize;
    std::optional<bool> resumable;
    Validators validators;
    std::optional<std::string> contentType;

    std::optional<int> maxConnections;
    std::optional<std::int64_t> speedLimitBytesPerSecond;

    std::optional<std::string> loginUser;
    std::vector<std::uint8_t> loginSecret;  // DPAPI-protected; empty when there is no password
    // DPAPI-protected cookie lines from the browser that captured this download (model::CookiesToText);
    // empty for downloads added in LDM. Removed once the download completes.
    std::vector<std::uint8_t> cookieSecret;
    // A download that came from submitting a form (checklist ENG-11): the DPAPI-protected request
    // body and its Content-Type. Empty for ordinary downloads; removed once the download completes.
    std::vector<std::uint8_t> postSecret;
    std::optional<std::string> postContentType;

    CompletionAction completionAction = CompletionAction::None;

    std::int64_t createdAtMs = 0;
    std::optional<std::int64_t> lastTryAtMs;
    std::optional<std::int64_t> completedAtMs;
    std::optional<std::string> lastResult;
    std::int64_t sortOrder = 0;

    // Membership of a download queue (checklist Q-01): the queue, the place in it, and the failed
    // attempts in the queue's current run, which decide when the queue gives up on it (Q-06).
    std::optional<QueueId> queueId;
    std::int64_t queuePosition = 0;
    int queueFailures = 0;

    // A Site Grabber download (M4 design section 6.3): its project, and whether the main list
    // shows it. A project's downloads are hidden until the user adds them to the main list.
    std::optional<GrabberProjectId> projectId;
    bool listed = true;

    // A file already at the destination is replaced when this download completes: a Redownload, a
    // changed file of the synchronization queue, or the user's choice when it was added. Stored, so
    // a download continued after a crash still replaces the old file instead of taking a new name.
    bool replaceFinalFile = false;

    // A file's byte ranges. Empty for a stream, whose progress is in `tracks`.
    std::vector<Segment> segments;

    DownloadKind kind = DownloadKind::File;
    // A stream whose manifest came from a loopback or private address: its pieces may be on such
    // addresses too. Otherwise LDM refuses them, so a hostile manifest cannot make LDM send
    // requests into the home network.
    bool mediaFromPrivateNetwork = false;
    // A video of a supported site (a Dash stream of whole-file or piece-list tracks, or an Hls
    // stream of playlists): `url` is the video's page,
    // as core::CheckSitePage gives it, and the tracks' addresses come from asking the site
    // resolver about that page. They expire, so they are asked for again when they do.
    bool siteVideo = false;
    // A site video added from a list without a quality chosen yet: it has no tracks until it first
    // starts, when it asks the resolver about its page and picks one this way. Kept afterwards.
    std::optional<SiteQuality> siteQuality;
    std::vector<MediaTrack> tracks;  // a stream's tracks, in order; empty for a file

    [[nodiscard]] bool IsStream() const noexcept { return kind != DownloadKind::File; }
    // Shown in the main list: every download but a Grabber project's that was not added to it. A
    // download whose project is gone shows, so nothing stays hidden for good.
    [[nodiscard]] bool InMainList() const noexcept { return listed || !projectId; }
    [[nodiscard]] std::int64_t DownloadedBytes() const noexcept;
};

}  // namespace ldm::model
