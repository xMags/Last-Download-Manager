#pragma once

#include "core/SegmentPlanner.h"
#include "core/TransferEngine.h"
#include "engine/BufferPool.h"
#include "engine/ConnectionGuards.h"
#include "engine/CurlHandles.h"
#include "engine/DiskWriter.h"
#include "engine/HttpHeaders.h"
#include "engine/Run.h"
#include "engine/TokenBucket.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace ldm::engine {

// Tunable constants of the connection strategy (design section 4). Defaults come from measured
// behavior of established download managers; tests shorten the timings.
struct TransferTuning {
    int initialExtraConnections = 3;
    std::chrono::milliseconds rampInterval{250};
    std::chrono::milliseconds progressInterval{500};
    std::int64_t overlapBytes = 64;
    std::int64_t writeChunkBytes = 256 * 1024;
    core::PlannerConfig planner;
    long connectTimeoutSeconds = 30;
    long stallTimeoutSeconds = 60;  // no byte for this long counts as a timeout
};

// Services shared by every transfer, owned by the network loop.
struct TransferServices {
    CURLM* multi = nullptr;
    DiskWriter* disk = nullptr;
    BufferPool* pool = nullptr;
    TokenBucket* globalBucket = nullptr;
    // A stream's own limit, shared by its pieces and its whole-file tracks; none for a file,
    // whose limit is its transfer's own.
    TokenBucket* runBucket = nullptr;
    core::EngineListener* listener = nullptr;
    std::mt19937* random = nullptr;
    const TransferTuning* tuning = nullptr;
};

// One run of one download on the network thread (design section 4): the first GET, the ramp of
// extra connections, dynamic splitting, overlap verification, retries, rate limiting and the
// hand-off to the disk thread. Not thread-safe; every method runs on the network thread.
//
// libcurl callbacks only record data and flags. Anything that adds or removes connections happens
// in Service(), so no callback ever destroys the object that is calling it.
//
// A stream's whole-file track runs as a Transfer inside its MediaRun, writing file `file` of the
// run and reporting to the MediaRun through `services.listener`.
class Transfer final : public Run, public ConnectionOwner {
public:
    Transfer(const TransferServices& services, core::TransferRequest request, std::uint32_t file = 0);
    ~Transfer() override;

    Transfer(const Transfer&) = delete;
    Transfer& operator=(const Transfer&) = delete;

    [[nodiscard]] RunKey Key() const noexcept override { return RunKey{request_.id, request_.epoch, file_}; }

    void Begin(Clock::time_point now);
    void RequestStop() override;
    void SetSpeedLimit(std::optional<std::int64_t> bytesPerSecond, Clock::time_point now) override;
    // A file has no playlist to grow: the engine only extends stream runs, so this does nothing.
    void ExtendMedia(core::MediaExtension extension, Clock::time_point now) override;

    // A connection's easy handle reported CURLMSG_DONE. `easy` belongs to this transfer.
    void OnConnectionDone(CURL* easy, CURLcode result, Clock::time_point now) override;
    void OnDiskReport(DiskReport& report, Clock::time_point now) override;

    void Service(Clock::time_point now) override;
    [[nodiscard]] Clock::time_point NextDeadline(Clock::time_point now) const override;

    // True once TransferFinished was sent; the loop then destroys the transfer.
    [[nodiscard]] bool Done() const noexcept override { return phase_ == Phase::Done; }

    // libcurl entry points; `userdata` is the Connection.
    static size_t WriteCallback(char* data, size_t size, size_t count, void* userdata);
    static size_t HeaderCallback(char* data, size_t size, size_t count, void* userdata);

private:
    enum class Phase {
        Starting,   // waiting for the first accepted response
        Running,
        Closing,    // connections gone; waiting for the disk thread to close the file
        Done,
    };

    struct Segment {
        std::uint32_t key = 0;
        std::int64_t start = 0;
        std::optional<std::int64_t> end;  // exclusive
        std::int64_t position = 0;        // next byte to receive
        std::int64_t durableEnd = 0;      // bytes before this offset are flushed
        bool busy = false;                // a connection serves this segment
        int attempts = 0;                 // consecutive failed attempts
        std::optional<Clock::time_point> retryAt;
        // First bytes of the segment as received by its own connection, and bytes the previous
        // segment's connection read past its end. Compared to verify the overlap (ENG-03).
        std::vector<std::byte> head;
        bool headFromStart = false;
        std::vector<std::byte> overlapFromPrevious;

        [[nodiscard]] std::optional<std::int64_t> Remaining() const noexcept
        {
            if (!end) {
                return std::nullopt;
            }
            return *end > position ? *end - position : 0;
        }
        [[nodiscard]] bool Received() const noexcept { return end.has_value() && position >= *end; }
    };

    struct Connection {
        Transfer* owner = nullptr;
        CurlEasy easy;
        CurlSlist headerList;
        std::array<char, CURL_ERROR_SIZE> errorBuffer{};
        AddressGuard addressGuard;
        std::uint32_t segmentKey = 0;
        std::int64_t requestStart = 0;
        bool probe = false;  // the first connection of the run; its response is validated in full
        bool ranged = false;

        ResponseHeaders headers;
        bool accepted = false;
        bool reachedEnd = false;     // stopped on purpose at the segment end
        bool rangeRejected = false;  // server answered 200 to a range request
        std::optional<core::TransferFailure> failure;

        std::int64_t position = 0;
        std::vector<std::byte> pending;  // received, reserved in the pool, not yet sent to disk
        std::vector<std::byte> overlap;  // bytes past the segment end
        bool pausedForRate = false;
        bool pausedForMemory = false;
    };

    size_t OnData(Connection& connection, const char* data, size_t size);
    bool AcceptResponse(Connection& connection);
    bool AcceptFirstResponse(Connection& connection, Segment& segment);
    bool AcceptExtraResponse(Connection& connection, Segment& segment);
    [[nodiscard]] bool IsHttp() const noexcept { return http_; }

    void StartConnection(Segment& segment, bool probe, Clock::time_point now);
    void RemoveConnection(Connection& connection);
    void FlushPending(Connection& connection);
    void CheckOverlap(Segment& previous, Segment& next);
    void FillSlots(Clock::time_point now);
    void UnpauseConnections(Clock::time_point now);
    void HandleFailure(Segment& segment, core::TransferFailure failure, std::optional<int> retryAfter,
                       Clock::time_point now);
    void Fail(core::TransferFailure failure);
    void BeginClosing(core::TransferOutcome outcome);
    void MaybeComplete();
    void ReportResponseInfo();
    void ReportProgress(Clock::time_point now);

    [[nodiscard]] Segment* FindSegment(std::uint32_t key);
    [[nodiscard]] Segment* SegmentStartingAt(std::int64_t offset);
    [[nodiscard]] Segment* SegmentEndingAt(std::int64_t offset);
    [[nodiscard]] Connection* FindConnection(CURL* easy);
    [[nodiscard]] std::vector<model::Segment> DurableSegments() const;
    [[nodiscard]] bool HasProgress() const noexcept;
    [[nodiscard]] std::optional<std::string> IfRangeValue() const;
    [[nodiscard]] int ConnectionLimit() const noexcept;
    [[nodiscard]] bool CanTake(Clock::time_point now) noexcept;

    TransferServices services_;
    core::TransferRequest request_;
    std::uint32_t file_ = 0;
    bool http_ = true;
    bool resumeRun_ = false;  // the run continues stored progress
    // Synchronization: the first request asked whether the file changed; the answer was no.
    bool conditionalRequest_ = false;
    bool unchangedOnServer_ = false;

    Phase phase_ = Phase::Starting;
    // Set by callbacks and commands; acted on in Service(), where connections may be removed.
    std::optional<core::TransferOutcome> closeRequested_;
    core::TransferOutcome closingOutcome_ = core::TransferOutcome::Failed;
    std::optional<core::TransferFailure> failure_;
    std::optional<core::TransferFailure> lastRetry_;

    std::vector<Segment> segments_;
    std::uint32_t nextSegmentKey_ = 0;
    std::vector<std::unique_ptr<Connection>> connections_;
    // Where the next unpause pass starts. The first connection woken after a refill takes the
    // tokens, so starting one later each time shares a limited rate evenly (P-04).
    size_t unpauseRotation_ = 0;

    // Facts established by the first accepted response.
    std::optional<std::int64_t> totalSize_;
    bool rangesPossible_ = false;
    bool resumable_ = false;
    model::Validators validators_;
    std::optional<std::string> contentType_;
    std::optional<std::string> fileName_;
    std::string finalUrl_;
    bool responseInfoDirty_ = false;

    TokenBucket bucket_;
    int allowedConnections_ = 1;
    Clock::time_point nextRampAt_{};
    Clock::time_point nextProgressAt_{};
    std::int64_t receivedThisRun_ = 0;
    std::deque<std::pair<Clock::time_point, std::int64_t>> speedSamples_;
};

}  // namespace ldm::engine
