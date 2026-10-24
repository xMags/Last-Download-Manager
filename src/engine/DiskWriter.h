#pragma once

#include "core/TransferEngine.h"
#include "engine/BufferPool.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>
#include <vector>

namespace ldm::engine {

// Identifies one file of one run of one download. Reports for an older epoch are ignored by the
// receiver. A file download has one file, 0; a stream has one per piece and per whole-file track.
struct RunKey {
    model::DownloadId id;
    std::int64_t epoch = 0;
    std::uint32_t file = 0;

    auto operator<=>(const RunKey&) const = default;
};

// Per segment, the absolute file offset up to which bytes have been written contiguously.
// Segments are keyed by a stable number that the network thread assigns.
using Watermarks = std::map<std::uint32_t, std::int64_t>;

namespace disk {

// Bytes up to the watermarks are flushed to the device.
struct Durable {
    Watermarks watermarks;
};
// The file is flushed and closed. `failure` is set when the final flush failed; the watermarks
// are then the last ones that were durable.
struct Closed {
    Watermarks watermarks;
    std::optional<core::TransferFailure> failure;
};
// Opening, sizing or writing failed. Further writes for this run are dropped until Close.
struct Failed {
    core::TransferFailure failure;
};

}  // namespace disk

struct DiskReport {
    RunKey key;
    std::variant<disk::Durable, disk::Closed, disk::Failed> payload;
};

enum class OpenMode {
    CreateNew,  // truncate or create: a fresh run
    Resume,     // must exist and hold at least `requiredLength` bytes
};

// Owns every open partial file and performs all file I/O on its own thread (design section 3):
// positional writes, a flush at most every two seconds per file, durable watermarks after each
// flush, and the final move. Every method is thread-safe and only enqueues.
//
// Reports and finalize results are delivered on the disk thread through the callbacks given to
// the constructor; they must only enqueue.
class DiskWriter {
public:
    using ReportSink = std::function<void(DiskReport)>;
    using FinalizeSink = std::function<void(core::EngineEvent)>;

    DiskWriter(BufferPool& pool, ReportSink reportSink, FinalizeSink finalizeSink,
               std::chrono::milliseconds flushInterval = std::chrono::seconds(2));
    // Drains queued jobs, flushes and closes every file, then joins the thread.
    ~DiskWriter();

    DiskWriter(const DiskWriter&) = delete;
    DiskWriter& operator=(const DiskWriter&) = delete;

    void Open(RunKey key, std::filesystem::path path, OpenMode mode, std::int64_t requiredLength);
    // Sets the final length once it is known; marks the file sparse where the volume allows.
    void SetLength(RunKey key, std::int64_t length);
    // Takes ownership of `data`; its size was reserved in the buffer pool and is released after
    // the write, whether or not it succeeds.
    void Write(RunKey key, std::uint32_t segment, std::int64_t offset, std::vector<std::byte> data);
    void Close(RunKey key);
    void Finalize(core::FinalizeRequest request);

private:
    struct OpenJob {
        RunKey key;
        std::filesystem::path path;
        OpenMode mode;
        std::int64_t requiredLength;
    };
    struct SetLengthJob {
        RunKey key;
        std::int64_t length;
    };
    struct WriteJob {
        RunKey key;
        std::uint32_t segment;
        std::int64_t offset;
        std::vector<std::byte> data;
    };
    struct CloseJob {
        RunKey key;
    };
    struct FinalizeJob {
        core::FinalizeRequest request;
    };
    using Job = std::variant<OpenJob, SetLengthJob, WriteJob, CloseJob, FinalizeJob>;

    struct OpenFile;

    void Enqueue(Job job);
    void Run(std::stop_token stopToken);
    void Handle(OpenJob& job);
    void Handle(SetLengthJob& job);
    void Handle(WriteJob& job);
    void Handle(CloseJob& job);
    void Handle(FinalizeJob& job);
    void FlushDue(std::chrono::steady_clock::time_point now);
    void Fail(RunKey key, OpenFile& file, core::TransferFailure failure);

    BufferPool& pool_;
    const ReportSink reportSink_;
    const FinalizeSink finalizeSink_;
    const std::chrono::milliseconds flushInterval_;

    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::vector<Job> pending_;

    // Disk thread only.
    std::map<RunKey, std::unique_ptr<OpenFile>> files_;

    std::jthread thread_;  // last member: starts after everything above is constructed
};

}  // namespace ldm::engine
