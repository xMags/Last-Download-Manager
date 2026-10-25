#pragma once

#include "core/TransferEngine.h"
#include "engine/Aes128Cbc.h"
#include "engine/ConnectionGuards.h"
#include "engine/CurlHandles.h"
#include "engine/HttpHeaders.h"
#include "engine/Run.h"
#include "engine/TokenBucket.h"
#include "engine/Transfer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ldm::engine {

// One run of one stream download on the network thread (M3 design section 6): the pieces of every
// track as short transfers, several at once up to the connection limit, each into its own file,
// lowest playback position first; AES-128 keys fetched before the pieces that need them and
// pieces decrypted as they arrive, so only plaintext reaches the disk; retries per piece; the
// download's own and the global rate limit. A track that is one file runs as a Transfer inside.
//
// A piece counts as done once the disk thread closed its file with every byte flushed; only
// those are reported, so the core may store them as they come.
//
// Not thread-safe; every method runs on the network thread. libcurl callbacks only record data
// and flags; connections are added and removed in Service() and OnConnectionDone().
class MediaRun final : public Run, public ConnectionOwner {
public:
    MediaRun(const TransferServices& services, core::MediaRequest request);
    ~MediaRun() override;

    MediaRun(const MediaRun&) = delete;
    MediaRun& operator=(const MediaRun&) = delete;

    [[nodiscard]] RunKey Key() const noexcept override { return RunKey{request_.id, request_.epoch, 0}; }

    void Begin(Clock::time_point now);
    void RequestStop() override;
    void SetSpeedLimit(std::optional<std::int64_t> bytesPerSecond, Clock::time_point now) override;
    // Appends a live recording's new keys and pieces to their tracks; they are fetched after
    // everything the run already had. A run whose tracks are all done and still live idles,
    // reporting progress, until an extension brings pieces or ends the recording.
    void ExtendMedia(core::MediaExtension extension, Clock::time_point now) override;
    void OnConnectionDone(CURL* easy, CURLcode result, Clock::time_point now) override;
    void OnDiskReport(DiskReport& report, Clock::time_point now) override;
    void Service(Clock::time_point now) override;
    [[nodiscard]] Clock::time_point NextDeadline(Clock::time_point now) const override;
    [[nodiscard]] bool Done() const noexcept override { return phase_ == Phase::Done; }

    // A key is 16 bytes; a server sending more than this is not sending a key.
    static constexpr size_t kMaxKeyBytes = 64;

private:
    enum class Phase {
        Running,
        Closing,  // connections gone; waiting for the disk thread and whole-file tracks
        Done,
    };

    enum class PieceState {
        Waiting,   // not started, or waiting to retry
        Fetching,  // a connection serves it
        Closing,   // every byte received; waiting for its file to be closed and flushed
        Durable,
    };

    struct Piece {
        size_t track = 0;  // index into request_.tracks
        size_t index = 0;  // into that track's pieces
        PieceState state = PieceState::Waiting;
        int attempts = 0;
        std::optional<Clock::time_point> retryAt;
        std::uint32_t file = 0;    // of its current attempt
        std::int64_t bytes = 0;    // plaintext bytes written by its current attempt
    };

    struct KeyState {
        std::optional<model::AesKey> value;
        bool fetching = false;
        int attempts = 0;
        std::optional<Clock::time_point> retryAt;
    };
    using KeySlot = std::pair<size_t, std::uint32_t>;  // track, key index

    struct Connection {
        enum class Kind {
            Piece,
            Key,
        };
        MediaRun* owner = nullptr;
        Kind kind = Kind::Piece;
        size_t piece = 0;  // index into pieces_
        KeySlot key{};
        CurlEasy easy;
        CurlSlist headerList;
        std::array<char, CURL_ERROR_SIZE> errorBuffer{};
        AddressGuard addressGuard;
        ResponseHeaders headers;
        bool accepted = false;
        std::optional<core::TransferFailure> failure;

        bool ranged = false;
        // A server answering a range with the whole file: the bytes before the range are skipped
        // and the transfer ends after it.
        std::int64_t skip = 0;
        std::optional<std::int64_t> remaining;
        bool reachedEnd = false;
        std::int64_t received = 0;  // from the network, before decryption
        std::vector<std::byte> pending;  // plaintext, reserved in the pool, not yet sent to disk
        std::optional<Aes128CbcDecryptor> decryptor;
        // Decryption holds back the last block; this much stays reserved in the pool for it from
        // the first byte until the piece is finished.
        bool slackReserved = false;
        std::string keyBody;
        bool pausedForRate = false;
        bool pausedForMemory = false;
    };

    // A whole-file track: a Transfer whose events come here instead of the core.
    struct WholeFile;
    class WholeFileListener;

    static size_t WriteCallback(char* data, size_t size, size_t count, void* userdata);
    static size_t KeyWriteCallback(char* data, size_t size, size_t count, void* userdata);
    static size_t HeaderCallback(char* data, size_t size, size_t count, void* userdata);

    size_t OnData(Connection& connection, const char* data, size_t size);
    bool Accept(Connection& connection);
    [[nodiscard]] const core::MediaPieceRequest& RequestOf(const Piece& piece) const;
    [[nodiscard]] bool CanTake(Clock::time_point now) noexcept;

    CurlEasy NewEasy(Connection& connection, const std::string& url);
    void StartPiece(size_t index, Clock::time_point now);
    void StartKey(const KeySlot& slot);
    void FinishPiece(Connection& connection, CURLcode result, Clock::time_point now);
    void FinishKey(Connection& connection, CURLcode result, Clock::time_point now);
    void RemoveConnection(Connection& connection);
    void FlushPending(Connection& connection);
    void Retry(int& attempts, std::optional<Clock::time_point>& retryAt, core::TransferFailure failure,
               std::optional<int> retryAfter, Clock::time_point now);
    [[nodiscard]] std::optional<core::TransferFailure> ConnectionFailure(const Connection& connection,
                                                                        CURLcode result) const;

    void FillSlots(Clock::time_point now);
    void UnpauseConnections(Clock::time_point now);
    void Fail(core::TransferFailure failure);
    void BeginClosing(core::TransferOutcome outcome);
    void MaybeFinishClosing();
    void ReportProgress(Clock::time_point now);
    [[nodiscard]] std::vector<core::MediaTrackRanges> WholeFileRanges() const;
    [[nodiscard]] bool AllDone() const;

    TransferServices services_;
    core::MediaRequest request_;
    Phase phase_ = Phase::Running;
    std::optional<core::TransferOutcome> closeRequested_;
    core::TransferOutcome closingOutcome_ = core::TransferOutcome::Failed;
    std::optional<core::TransferFailure> failure_;
    std::optional<core::TransferFailure> lastRetry_;

    // In the order they are fetched: playback position, tracks interleaved.
    std::vector<Piece> pieces_;
    size_t firstWaiting_ = 0;  // no piece before this one is waiting, except those to retry
    std::vector<size_t> retrying_;  // pieces waiting to retry, wherever they are
    size_t durablePieces_ = 0;
    std::map<KeySlot, KeyState> keys_;
    std::map<std::uint32_t, size_t> openFiles_;  // file of a piece attempt, until it is closed
    std::vector<std::unique_ptr<WholeFile>> wholeFiles_;
    std::vector<std::unique_ptr<Connection>> connections_;
    std::uint32_t nextFile_ = 1;  // 0 is the run itself
    size_t unpauseRotation_ = 0;
    int pieceConnections_ = 1;  // of maxConnections, what the pieces may use beside whole-file tracks

    std::vector<core::MediaPieceDone> doneSinceReport_;
    TokenBucket bucket_;
    Clock::time_point nextProgressAt_{};
    std::int64_t receivedThisRun_ = 0;
    std::deque<std::pair<Clock::time_point, std::int64_t>> speedSamples_;
};

}  // namespace ldm::engine
