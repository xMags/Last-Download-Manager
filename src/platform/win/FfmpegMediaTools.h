#pragma once

#include "core/MediaTools.h"
#include "core/TransferEngine.h"

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <variant>

namespace ldm::platform {

// The media tools port with the ffmpeg.exe and ffprobe.exe that ship next to ldm.exe (M3 design
// section 6). One join at a time on this object's own thread: local playlists for the pieces, a
// stream copy into MP4 (Matroska when MP4 cannot hold the streams), WebVTT for subtitles, and
// ffprobe for the description. FFmpeg reads local files only and runs in its own process with
// the limits of RunChildProcess.
//
// Threads: Mux(), Describe() and Cancel() are called on the core thread; answers go to `listener`
// from the worker. Cancel ends a running FFmpeg or ffprobe through its stop event.
class FfmpegMediaTools final : public core::MediaTools {
public:
    // `toolsFolder` holds ffmpeg.exe and ffprobe.exe; `listener` must outlive this object.
    FfmpegMediaTools(std::filesystem::path toolsFolder, core::EngineListener& listener);
    // Ends a running FFmpeg and joins the worker; jobs still queued are dropped unanswered, as
    // the core is shutting down.
    ~FfmpegMediaTools() override;
    FfmpegMediaTools(const FfmpegMediaTools&) = delete;
    FfmpegMediaTools& operator=(const FfmpegMediaTools&) = delete;

    void Mux(core::MuxRequest request) override;
    void Describe(core::DescribeRequest request) override;
    void Cancel(model::DownloadId id, std::int64_t epoch) override;

    // A join of a long video copies gigabytes; this is far more than any needs.
    static constexpr std::chrono::minutes kJoinTimeout{60};

private:
    using JobKey = std::pair<model::DownloadId, std::int64_t>;
    using Job = std::variant<core::MuxRequest, core::DescribeRequest>;

    [[nodiscard]] static JobKey KeyOf(const Job& job);
    // The answer for `job` when it was stopped before or while it ran.
    [[nodiscard]] static core::EnginePayload StoppedAnswer(const Job& job);
    void Enqueue(Job job);
    void RunWorker();
    [[nodiscard]] core::MuxFinished Process(const core::MuxRequest& request, HANDLE stop) const;
    [[nodiscard]] core::MuxFinished JoinStreams(const core::MuxRequest& request, HANDLE stop) const;
    [[nodiscard]] core::MuxFinished ConvertSubtitles(const core::MuxRequest& request, HANDLE stop) const;
    [[nodiscard]] std::string DescribeFile(const std::filesystem::path& file, HANDLE stop) const;

    const std::filesystem::path ffmpeg_;
    const std::filesystem::path ffprobe_;
    core::EngineListener& listener_;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::optional<JobKey> current_;
    HANDLE currentStop_ = nullptr;  // manual-reset event of the running job, owned by the worker
    bool stopping_ = false;
    std::thread worker_;  // last: starts after everything above exists
};

}  // namespace ldm::platform
