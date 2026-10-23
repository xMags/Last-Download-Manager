#pragma once

#include "model/Ids.h"

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

// A byte range of the file assigned to one connection. `end` is exclusive and absent while the
// total size is unknown. `done` counts bytes from `start` that are durably on disk.
struct Segment {
    std::int64_t start = 0;
    std::optional<std::int64_t> end;
    std::int64_t done = 0;

    [[nodiscard]] std::int64_t WriteOffset() const noexcept { return start + done; }
    [[nodiscard]] bool IsComplete() const noexcept { return end.has_value() && start + done >= *end; }
};

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

    std::vector<Segment> segments;

    [[nodiscard]] std::int64_t DownloadedBytes() const noexcept;
};

}  // namespace ldm::model
