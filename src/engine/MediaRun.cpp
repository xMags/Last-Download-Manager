#include "engine/MediaRun.h"

#include "engine/FailurePolicy.h"

#include <algorithm>
#include <cassert>
#include <random>
#include <utility>
#include <variant>

namespace ldm::engine {

namespace {

using core::FailureClass;
using core::FailureReason;
using core::TransferFailure;

TransferFailure MakeFailure(FailureReason reason, FailureClass failureClass, std::string message)
{
    return TransferFailure{reason, failureClass, 0, std::move(message)};
}

// Decryption can hold back one block, so a chunk may produce up to this much more plaintext
// than it had ciphertext.
constexpr std::int64_t kDecryptSlack = 16;

}  // namespace

// A whole-file track and what its Transfer last reported.
struct MediaRun::WholeFile {
    std::uint32_t track = 0;  // the track's number in the download
    std::unique_ptr<WholeFileListener> listener;
    std::unique_ptr<Transfer> transfer;
    std::vector<model::Segment> ranges;
    std::optional<std::int64_t> totalSize;
    // The Transfer counts every byte of the file, those of earlier runs too; the run reports only
    // what arrived since `startBytes`, as the coordinator adds what was on disk itself.
    std::int64_t startBytes = 0;
    std::int64_t receivedBytes = 0;
    std::int64_t bytesPerSecond = 0;
    int connections = 0;
    std::optional<core::TransferFinished> finished;
};

// Receives a whole-file track's events. The Transfer calls it on the network thread from inside
// its own methods, so it only records.
class MediaRun::WholeFileListener final : public core::EngineListener {
public:
    WholeFileListener(MediaRun& run, WholeFile& wholeFile) : run_(run), wholeFile_(wholeFile) {}

    void OnEngineEvent(core::EngineEvent event) override
    {
        if (auto* progress = std::get_if<core::ProgressInfo>(&event.payload)) {
            wholeFile_.ranges = std::move(progress->segments);
            wholeFile_.totalSize = progress->totalSize;
            wholeFile_.receivedBytes = progress->receivedBytes;
            wholeFile_.bytesPerSecond = progress->bytesPerSecond;
            wholeFile_.connections = static_cast<int>(progress->connections.size());
            if (progress->lastRetry) {
                run_.lastRetry_ = progress->lastRetry;
            }
            return;
        }
        if (auto* finished = std::get_if<core::TransferFinished>(&event.payload)) {
            wholeFile_.ranges = finished->segments;
            wholeFile_.totalSize = finished->totalSize;
            wholeFile_.connections = 0;
            wholeFile_.bytesPerSecond = 0;
            wholeFile_.finished = std::move(*finished);
        }
        // ResponseInfo says nothing a stream needs: its name and type come from the manifest.
    }

private:
    MediaRun& run_;
    WholeFile& wholeFile_;
};

MediaRun::MediaRun(const TransferServices& services, core::MediaRequest request)
    : services_(services), request_(std::move(request))
{
}

MediaRun::~MediaRun()
{
    for (const auto& connection : connections_) {
        curl_multi_remove_handle(services_.multi, connection->easy.get());
        services_.pool->Release(static_cast<std::int64_t>(connection->pending.size()) +
                                (connection->slackReserved ? kDecryptSlack : 0));
    }
}

const core::MediaPieceRequest& MediaRun::RequestOf(const Piece& piece) const
{
    return request_.tracks[piece.track].pieces[piece.index];
}

void MediaRun::Begin(Clock::time_point now)
{
    bucket_.SetRate(request_.speedLimitBytesPerSecond, now);
    nextProgressAt_ = now + services_.tuning->progressInterval;

    int wholeFileTracks = 0;
    bool pieceTracks = false;
    for (const core::MediaTrackRequest& track : request_.tracks) {
        wholeFileTracks += track.wholeFile ? 1 : 0;
        pieceTracks = pieceTracks || !track.wholeFile;
    }
    // The run's connections are shared: each whole-file track takes a share, and the pieces of
    // all other tracks together take one, so the run never has more than maxConnections (at
    // least one each).
    const int maxConnections = std::max(1, request_.maxConnections);
    const int share = std::max(1, maxConnections / std::max(1, wholeFileTracks + (pieceTracks ? 1 : 0)));
    pieceConnections_ = std::max(1, maxConnections - share * wholeFileTracks);
    // Pieces of all tracks in playback order: each track's pieces spread evenly over [0, 1).
    std::vector<std::pair<double, Piece>> ordered;
    for (size_t t = 0; t < request_.tracks.size(); ++t) {
        const core::MediaTrackRequest& track = request_.tracks[t];
        if (track.wholeFile) {
            auto wholeFile = std::make_unique<WholeFile>();
            wholeFile->track = track.track;
            wholeFile->listener = std::make_unique<WholeFileListener>(*this, *wholeFile);
            wholeFile->ranges = track.ranges;
            for (const model::Segment& range : track.ranges) {
                wholeFile->startBytes += range.done;
            }
            wholeFile->receivedBytes = wholeFile->startBytes;
            TransferServices inner = services_;
            inner.listener = wholeFile->listener.get();
            inner.runBucket = &bucket_;
            core::TransferRequest file;
            file.id = request_.id;
            file.epoch = request_.epoch;
            file.url = track.url;
            file.referer = request_.referer;
            file.userAgent = request_.userAgent;
            file.cookieLines = request_.cookieLines;
            file.headers = request_.headers;
            file.proxy = request_.proxy;
            file.partialPath = track.path;
            file.segments = track.ranges;
            const bool bounded = !track.ranges.empty() && std::all_of(track.ranges.begin(), track.ranges.end(),
                                                                       [](const model::Segment& s) { return s.end.has_value(); });
            if (bounded) {
                std::int64_t end = 0;
                for (const model::Segment& segment : track.ranges) {
                    end = std::max(end, *segment.end);
                }
                file.knownSize = end;
                file.resumable = true;
            }
            file.maxRequestBytes = track.maxRequestBytes;
            file.maxConnections = share;
            file.maxAttemptsPerSegment = request_.maxAttemptsPerSegment;
            file.refusePrivateAddresses = request_.refusePrivateAddresses;
            wholeFile->transfer = std::make_unique<Transfer>(inner, std::move(file), nextFile_++);
            wholeFiles_.push_back(std::move(wholeFile));
            continue;
        }
        for (std::uint32_t k = 0; k < track.keys.size(); ++k) {
            KeyState state;
            state.value = track.keys[k].value;
            keys_.emplace(KeySlot{t, k}, state);
        }
        const size_t count = track.pieces.size();
        for (size_t i = 0; i < count; ++i) {
            Piece piece;
            piece.track = t;
            piece.index = i;
            ordered.emplace_back((static_cast<double>(i) + 0.5) / static_cast<double>(count), piece);
        }
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    pieces_.reserve(ordered.size());
    for (auto& [position, piece] : ordered) {
        pieces_.push_back(piece);
    }
    for (const auto& wholeFile : wholeFiles_) {
        wholeFile->transfer->Begin(now);
    }
    FillSlots(now);
}

void MediaRun::RequestStop()
{
    if (phase_ != Phase::Running) {
        return;
    }
    if (!closeRequested_) {
        closeRequested_ = core::TransferOutcome::Stopped;
    }
}

void MediaRun::SetSpeedLimit(std::optional<std::int64_t> bytesPerSecond, Clock::time_point now)
{
    request_.speedLimitBytesPerSecond = bytesPerSecond;
    bucket_.SetRate(bytesPerSecond, now);
}

void MediaRun::ExtendMedia(core::MediaExtension extension, Clock::time_point now)
{
    if (phase_ != Phase::Running || closeRequested_) {
        return;
    }
    for (core::MediaTrackExtension& added : extension.tracks) {
        const auto found = std::find_if(request_.tracks.begin(), request_.tracks.end(), [&added](const auto& track) {
            return track.track == added.track && !track.wholeFile;
        });
        if (found == request_.tracks.end()) {
            continue;
        }
        const auto t = static_cast<size_t>(found - request_.tracks.begin());
        core::MediaTrackRequest& track = *found;
        for (model::MediaKey& key : added.keys) {
            KeyState state;
            state.value = key.value;
            keys_.emplace(KeySlot{t, static_cast<std::uint32_t>(track.keys.size())}, state);
            track.keys.push_back(std::move(key));
        }
        for (core::MediaPieceRequest& piece : added.pieces) {
            if (piece.key && *piece.key >= track.keys.size()) {
                Fail(MakeFailure(FailureReason::Internal, FailureClass::Fatal, "a new segment names a key the stream lacks"));
                return;
            }
            track.pieces.push_back(std::move(piece));
            Piece entry;
            entry.track = t;
            entry.index = track.pieces.size() - 1;
            pieces_.push_back(entry);
        }
        track.live = added.live;
    }
    FillSlots(now);
}

void MediaRun::Fail(TransferFailure failure)
{
    if (phase_ == Phase::Done) {
        return;
    }
    if (!failure_) {
        failure_ = std::move(failure);
    }
    if (phase_ == Phase::Closing) {
        closingOutcome_ = core::TransferOutcome::Failed;
        return;
    }
    closeRequested_ = core::TransferOutcome::Failed;
}

bool MediaRun::CanTake(Clock::time_point now) noexcept
{
    return bucket_.CanTake(now) && services_.globalBucket->CanTake(now);
}

CurlEasy MediaRun::NewEasy(Connection& connection, const std::string& url)
{
    CurlEasy easy(curl_easy_init());
    if (!easy) {
        return easy;
    }
    CURL* handle = easy.get();
    const TransferTuning& tuning = *services_.tuning;
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_PIPEWAIT, 0L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, tuning.connectTimeoutSeconds);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, tuning.stallTimeoutSeconds);
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, connection.errorBuffer.data());
    curl_easy_setopt(handle, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
    curl_easy_setopt(handle, CURLOPT_USERAGENT, request_.userAgent.c_str());
    if (request_.referer) {
        curl_easy_setopt(handle, CURLOPT_REFERER, request_.referer->c_str());
    }
    curl_easy_setopt(handle, CURLOPT_PROXY, request_.proxy ? request_.proxy->c_str() : "");
    if (!request_.cookieLines.empty()) {
        curl_easy_setopt(handle, CURLOPT_COOKIEFILE, "");  // turns on the cookie engine without reading a file
        for (const std::string& line : request_.cookieLines) {
            curl_easy_setopt(handle, CURLOPT_COOKIELIST, line.c_str());  // copied by libcurl
        }
    }
    curl_slist* headers = nullptr;
    for (const std::string& line : request_.headers) {
        headers = curl_slist_append(headers, line.c_str());
    }
    if (headers != nullptr) {
        connection.headerList.reset(headers);  // kept as long as the connection
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    }
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, &MediaRun::HeaderCallback);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &connection);
    ConnectionOwner::Attach(handle, this);
    connection.addressGuard.refuse = request_.refusePrivateAddresses && !request_.proxy;
    InstallAddressGuard(handle, &connection.addressGuard);
    return easy;
}

void MediaRun::StartPiece(size_t index, Clock::time_point now)
{
    (void)now;
    Piece& piece = pieces_[index];
    const core::MediaPieceRequest& request = RequestOf(piece);
    auto connection = std::make_unique<Connection>();
    connection->owner = this;
    connection->kind = Connection::Kind::Piece;
    connection->piece = index;
    if (request.key) {
        const auto key = keys_.find(KeySlot{piece.track, *request.key});
        assert(key != keys_.end() && key->second.value);
        connection->decryptor = Aes128CbcDecryptor::Create(*key->second.value, request.iv);
        if (!connection->decryptor) {
            Fail(MakeFailure(FailureReason::Internal, FailureClass::Fatal, "the decryption key could not be used"));
            return;
        }
    }
    connection->easy = NewEasy(*connection, request.url);
    if (!connection->easy) {
        Fail(MakeFailure(FailureReason::Internal, FailureClass::Fatal, "could not create a connection"));
        return;
    }
    CURL* easy = connection->easy.get();
    std::string range;
    if (request.rangeOffset && request.rangeLength) {
        connection->ranged = true;
        range = std::to_string(*request.rangeOffset) + "-" + std::to_string(*request.rangeOffset + *request.rangeLength - 1);
        curl_easy_setopt(easy, CURLOPT_RANGE, range.c_str());  // copied by libcurl
        // Ranges count bytes as stored; a compressed answer would not match them.
        curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "identity");
    } else {
        // Whatever libcurl can decode: subtitle and playlist-like pieces are often compressed.
        curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
    }
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &MediaRun::WriteCallback);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, connection.get());
    if (curl_multi_add_handle(services_.multi, easy) != CURLM_OK) {
        Fail(MakeFailure(FailureReason::Internal, FailureClass::Fatal, "could not start a connection"));
        return;
    }
    piece.file = nextFile_++;
    piece.bytes = 0;
    piece.state = PieceState::Fetching;
    piece.retryAt.reset();
    std::erase(retrying_, index);
    openFiles_[piece.file] = index;
    services_.disk->Open(RunKey{request_.id, request_.epoch, piece.file}, request.path, OpenMode::CreateNew, 0);
    connections_.push_back(std::move(connection));
}

void MediaRun::StartKey(const KeySlot& slot)
{
    KeyState& state = keys_.at(slot);
    const model::MediaKey& key = request_.tracks[slot.first].keys[slot.second];
    auto connection = std::make_unique<Connection>();
    connection->owner = this;
    connection->kind = Connection::Kind::Key;
    connection->key = slot;
    connection->easy = NewEasy(*connection, key.url);
    if (!connection->easy) {
        Fail(MakeFailure(FailureReason::Internal, FailureClass::Fatal, "could not create a connection"));
        return;
    }
    CURL* easy = connection->easy.get();
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &MediaRun::KeyWriteCallback);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, connection.get());
    if (curl_multi_add_handle(services_.multi, easy) != CURLM_OK) {
        Fail(MakeFailure(FailureReason::Internal, FailureClass::Fatal, "could not start a connection"));
        return;
    }
    state.fetching = true;
    state.retryAt.reset();
    connections_.push_back(std::move(connection));
}

size_t MediaRun::HeaderCallback(char* data, size_t size, size_t count, void* userdata)
{
    auto* connection = static_cast<Connection*>(userdata);
    const size_t length = size * count;
    connection->headers.ApplyLine(std::string_view(data, length));
    return length;
}

size_t MediaRun::KeyWriteCallback(char* data, size_t size, size_t count, void* userdata)
{
    auto* connection = static_cast<Connection*>(userdata);
    const size_t length = size * count;
    if (auto failure = ClassifyHttpStatus(connection->headers.status)) {
        connection->failure = std::move(failure);  // an error page, not a key
        return 0;
    }
    if (connection->keyBody.size() + length > kMaxKeyBytes) {
        connection->failure = MakeFailure(FailureReason::Protocol, FailureClass::Fatal,
                                          "the server sent more than a key for the stream's key address");
        return 0;
    }
    connection->keyBody.append(data, length);
    return length;
}

size_t MediaRun::WriteCallback(char* data, size_t size, size_t count, void* userdata)
{
    auto* connection = static_cast<Connection*>(userdata);
    return connection->owner->OnData(*connection, data, size * count);
}

bool MediaRun::Accept(Connection& connection)
{
    const int status = connection.headers.status;
    if (auto failure = ClassifyHttpStatus(status)) {
        connection.failure = std::move(failure);
        return false;
    }
    const core::MediaPieceRequest& request = RequestOf(pieces_[connection.piece]);
    if (connection.ranged) {
        if (status == 206) {
            const auto& range = connection.headers.contentRange;
            if (range && range->first != *request.rangeOffset) {
                connection.failure = MakeFailure(FailureReason::Protocol, FailureClass::Retryable,
                                                 "the server sent another part of the file than asked for");
                return false;
            }
        } else {
            // The whole file came back: keep only the asked part of it.
            connection.skip = *request.rangeOffset;
        }
        connection.remaining = *request.rangeLength;
    }
    connection.accepted = true;
    return true;
}

size_t MediaRun::OnData(Connection& connection, const char* data, size_t size)
{
    if (phase_ != Phase::Running || closeRequested_ || connection.reachedEnd) {
        return 0;
    }
    if (!connection.accepted && !Accept(connection)) {
        return 0;
    }
    const Clock::time_point now = Clock::now();
    if (!CanTake(now)) {
        connection.pausedForRate = true;
        return CURL_WRITEFUNC_PAUSE;
    }
    const auto chunk = static_cast<std::int64_t>(size);
    const std::int64_t reserved = chunk + kDecryptSlack;
    if (!services_.pool->TryReserve(reserved)) {
        connection.pausedForMemory = true;
        return CURL_WRITEFUNC_PAUSE;
    }
    bucket_.Take(chunk);
    services_.globalBucket->Take(chunk);
    connection.received += chunk;
    receivedThisRun_ += chunk;

    std::int64_t start = std::min(connection.skip, chunk);
    connection.skip -= start;
    std::int64_t length = chunk - start;
    if (connection.remaining) {
        length = std::min(length, *connection.remaining);
        *connection.remaining -= length;
        connection.reachedEnd = *connection.remaining == 0;
    }
    const auto* bytes = reinterpret_cast<const std::byte*>(data);  // char and std::byte alias by definition
    const std::span<const std::byte> slice(bytes + start, static_cast<size_t>(length));
    const size_t before = connection.pending.size();
    if (connection.decryptor) {
        if (!connection.decryptor->Update(slice, connection.pending)) {
            connection.pending.resize(before);
            services_.pool->Release(reserved);
            connection.failure = MakeFailure(FailureReason::Protocol, FailureClass::Fatal, "a segment could not be decrypted");
            return 0;
        }
    } else {
        connection.pending.insert(connection.pending.end(), slice.begin(), slice.end());
    }
    std::int64_t kept = static_cast<std::int64_t>(connection.pending.size() - before);
    if (connection.decryptor && !connection.slackReserved) {
        kept += kDecryptSlack;
        connection.slackReserved = true;
    }
    services_.pool->Release(reserved - kept);
    if (static_cast<std::int64_t>(connection.pending.size()) >= services_.tuning->writeChunkBytes) {
        FlushPending(connection);
    }
    return connection.reachedEnd ? 0 : size;
}

void MediaRun::FlushPending(Connection& connection)
{
    if (connection.pending.empty()) {
        return;
    }
    Piece& piece = pieces_[connection.piece];
    const auto size = static_cast<std::int64_t>(connection.pending.size());
    services_.disk->Write(RunKey{request_.id, request_.epoch, piece.file}, 0, piece.bytes, std::move(connection.pending));
    connection.pending = {};
    piece.bytes += size;
}

void MediaRun::RemoveConnection(Connection& connection)
{
    curl_multi_remove_handle(services_.multi, connection.easy.get());
    services_.pool->Release(static_cast<std::int64_t>(connection.pending.size()) +
                            (connection.slackReserved ? kDecryptSlack : 0));
    const auto it = std::find_if(connections_.begin(), connections_.end(),
                                 [&connection](const auto& candidate) { return candidate.get() == &connection; });
    if (it != connections_.end()) {
        connections_.erase(it);
    }
}

std::optional<TransferFailure> MediaRun::ConnectionFailure(const Connection& connection, CURLcode result) const
{
    if (connection.addressGuard.refuse && connection.addressGuard.reachedPrivate) {
        return MakeFailure(FailureReason::PrivateAddress, FailureClass::Fatal,
                           "the address leads to a loopback or private network address");
    }
    if (connection.failure) {
        return connection.failure;
    }
    if (connection.reachedEnd) {
        return std::nullopt;
    }
    return ClassifyCurlResult(static_cast<int>(result), connection.errorBuffer.data());
}

void MediaRun::Retry(int& attempts, std::optional<Clock::time_point>& retryAt, TransferFailure failure,
                     std::optional<int> retryAfter, Clock::time_point now)
{
    if (failure.failureClass != FailureClass::Retryable) {
        Fail(std::move(failure));
        return;
    }
    lastRetry_ = failure;
    attempts += 1;
    if (attempts >= request_.maxAttemptsPerSegment) {
        Fail(std::move(failure));
        return;
    }
    std::uniform_real_distribution<double> jitter(0.75, 1.25);
    retryAt = now + RetryDelay(attempts, jitter(*services_.random), retryAfter);
}

void MediaRun::OnConnectionDone(CURL* easy, CURLcode result, Clock::time_point now)
{
    const auto it = std::find_if(connections_.begin(), connections_.end(),
                                 [easy](const auto& connection) { return connection->easy.get() == easy; });
    if (it == connections_.end()) {
        return;
    }
    Connection& connection = **it;
    if (connection.kind == Connection::Kind::Key) {
        FinishKey(connection, result, now);
    } else {
        FinishPiece(connection, result, now);
    }
}

void MediaRun::FinishKey(Connection& connection, CURLcode result, Clock::time_point now)
{
    const KeySlot slot = connection.key;
    std::optional<TransferFailure> failure = ConnectionFailure(connection, result);
    if (!failure) {
        long status = 0;
        curl_easy_getinfo(connection.easy.get(), CURLINFO_RESPONSE_CODE, &status);
        failure = ClassifyHttpStatus(static_cast<int>(status));
    }
    if (!failure && connection.keyBody.size() != std::tuple_size_v<model::AesKey>) {
        failure = MakeFailure(FailureReason::Protocol, FailureClass::Fatal, "the stream's key is not 16 bytes");
    }
    std::string body = std::move(connection.keyBody);
    const std::optional<int> retryAfter = connection.headers.retryAfterSeconds;
    RemoveConnection(connection);  // `connection` is destroyed here

    KeyState& state = keys_.at(slot);
    state.fetching = false;
    if (phase_ != Phase::Running || closeRequested_) {
        return;
    }
    if (failure) {
        Retry(state.attempts, state.retryAt, std::move(*failure), retryAfter, now);
        return;
    }
    model::AesKey key{};
    std::transform(body.begin(), body.end(), key.begin(), [](char c) { return static_cast<std::uint8_t>(c); });
    state.value = key;
}

void MediaRun::FinishPiece(Connection& connection, CURLcode result, Clock::time_point now)
{
    if (!connection.accepted && !connection.failure && result == CURLE_OK && phase_ == Phase::Running &&
        !closeRequested_) {
        Accept(connection);  // an empty answer never reached OnData
    }
    const size_t index = connection.piece;
    std::optional<TransferFailure> failure = ConnectionFailure(connection, result);
    if (!failure && connection.remaining && *connection.remaining > 0) {
        failure = MakeFailure(FailureReason::ConnectionReset, FailureClass::Retryable,
                              "the server sent less of the segment than it should");
    }
    if (!failure && connection.decryptor) {
        const size_t before = connection.pending.size();
        if (!connection.decryptor->Finish(connection.pending)) {
            failure = MakeFailure(FailureReason::Protocol, FailureClass::Fatal, "a segment could not be decrypted");
        } else if (connection.slackReserved) {
            // The held-back plaintext now pending is paid from the standing reservation.
            services_.pool->Release(kDecryptSlack - static_cast<std::int64_t>(connection.pending.size() - before));
            connection.slackReserved = false;
        }
    }
    const std::optional<int> retryAfter = connection.headers.retryAfterSeconds;
    Piece& piece = pieces_[index];
    const RunKey file{request_.id, request_.epoch, piece.file};
    if (!failure) {
        FlushPending(connection);
        services_.disk->Close(file);
        piece.state = PieceState::Closing;
        RemoveConnection(connection);
        return;
    }
    RemoveConnection(connection);  // releases what was pending
    services_.disk->Close(file);   // the attempt's file is dropped; a retry writes it again
    piece.state = PieceState::Waiting;
    firstWaiting_ = std::min(firstWaiting_, index);
    if (phase_ != Phase::Running || closeRequested_) {
        return;
    }
    Retry(piece.attempts, piece.retryAt, std::move(*failure), retryAfter, now);
    if (piece.retryAt) {
        retrying_.push_back(index);
    }
}

void MediaRun::FillSlots(Clock::time_point now)
{
    if (phase_ != Phase::Running || closeRequested_) {
        return;
    }
    const auto limit = static_cast<size_t>(pieceConnections_);
    while (firstWaiting_ < pieces_.size() && pieces_[firstWaiting_].state != PieceState::Waiting) {
        ++firstWaiting_;
    }
    // Looks a bounded distance ahead, so a run with every slot taken costs nothing per call.
    constexpr size_t kLookAhead = 256;
    size_t looked = 0;
    for (size_t index = firstWaiting_; index < pieces_.size() && connections_.size() < limit && looked < kLookAhead;
         ++index) {
        Piece& piece = pieces_[index];
        if (piece.state != PieceState::Waiting) {
            continue;
        }
        ++looked;
        if (piece.retryAt && *piece.retryAt > now) {
            continue;
        }
        const core::MediaPieceRequest& request = RequestOf(piece);
        if (request.key) {
            const KeySlot slot{piece.track, *request.key};
            KeyState& key = keys_.at(slot);
            if (!key.value) {
                if (!key.fetching && (!key.retryAt || *key.retryAt <= now)) {
                    StartKey(slot);
                }
                continue;
            }
        }
        StartPiece(index, now);
        if (closeRequested_) {
            return;
        }
    }
}

void MediaRun::UnpauseConnections(Clock::time_point now)
{
    // Unpausing may call OnData synchronously; it only touches connection fields, never
    // connections_ itself, so iterating by index stays valid.
    const size_t count = connections_.size();
    const size_t first = count == 0 ? 0 : unpauseRotation_++ % count;
    for (size_t step = 0; step < count; ++step) {
        Connection& connection = *connections_[(first + step) % count];
        if (connection.pausedForRate) {
            if (CanTake(now)) {
                connection.pausedForRate = false;
                curl_easy_pause(connection.easy.get(), CURLPAUSE_CONT);
            }
        } else if (connection.pausedForMemory) {
            connection.pausedForMemory = false;
            curl_easy_pause(connection.easy.get(), CURLPAUSE_CONT);
        }
    }
}

bool MediaRun::AllDone() const
{
    const bool recording = std::any_of(request_.tracks.begin(), request_.tracks.end(),
                                       [](const core::MediaTrackRequest& track) { return track.live; });
    if (recording || durablePieces_ != pieces_.size()) {
        return false;
    }
    return std::all_of(wholeFiles_.begin(), wholeFiles_.end(), [](const auto& wholeFile) {
        return wholeFile->finished && wholeFile->finished->outcome == core::TransferOutcome::Completed;
    });
}

void MediaRun::Service(Clock::time_point now)
{
    if (phase_ == Phase::Done) {
        return;
    }
    for (const auto& wholeFile : wholeFiles_) {
        if (!wholeFile->transfer->Done()) {
            wholeFile->transfer->Service(now);
        }
    }
    if (phase_ == Phase::Closing) {
        MaybeFinishClosing();
        return;
    }
    for (const auto& wholeFile : wholeFiles_) {
        const auto& finished = wholeFile->finished;
        if (finished && finished->outcome != core::TransferOutcome::Completed && !closeRequested_) {
            Fail(finished->failure.value_or(
                MakeFailure(FailureReason::Internal, FailureClass::Fatal, "a track stopped without a reason")));
        }
    }
    if (closeRequested_) {
        BeginClosing(*closeRequested_);
        return;
    }
    UnpauseConnections(now);
    if (AllDone()) {
        BeginClosing(core::TransferOutcome::Completed);
        return;
    }
    FillSlots(now);
    if (closeRequested_) {
        BeginClosing(*closeRequested_);
        return;
    }
    if (now >= nextProgressAt_) {
        ReportProgress(now);
    }
}

void MediaRun::BeginClosing(core::TransferOutcome outcome)
{
    while (!connections_.empty()) {
        Connection& connection = *connections_.back();
        if (connection.kind == Connection::Kind::Piece) {
            Piece& piece = pieces_[connection.piece];
            services_.disk->Close(RunKey{request_.id, request_.epoch, piece.file});
            piece.state = PieceState::Waiting;
        } else {
            keys_.at(connection.key).fetching = false;
        }
        RemoveConnection(connection);
    }
    for (const auto& wholeFile : wholeFiles_) {
        wholeFile->transfer->RequestStop();
    }
    closeRequested_.reset();
    closingOutcome_ = failure_ ? core::TransferOutcome::Failed : outcome;
    phase_ = Phase::Closing;
    MaybeFinishClosing();
}

void MediaRun::MaybeFinishClosing()
{
    if (phase_ != Phase::Closing || !openFiles_.empty()) {
        return;
    }
    if (!std::all_of(wholeFiles_.begin(), wholeFiles_.end(), [](const auto& w) { return w->transfer->Done(); })) {
        return;
    }
    if (closingOutcome_ == core::TransferOutcome::Completed && !AllDone()) {
        failure_ = MakeFailure(FailureReason::Internal, FailureClass::Fatal, "not every byte reached the disk");
        closingOutcome_ = core::TransferOutcome::Failed;
    }
    core::MediaFinished finished;
    finished.outcome = failure_ ? core::TransferOutcome::Failed : closingOutcome_;
    finished.failure = finished.outcome == core::TransferOutcome::Failed ? failure_ : std::nullopt;
    finished.done = std::move(doneSinceReport_);
    doneSinceReport_.clear();
    finished.ranges = WholeFileRanges();
    phase_ = Phase::Done;
    services_.listener->OnEngineEvent(core::EngineEvent{request_.id, request_.epoch, std::move(finished)});
}

void MediaRun::OnDiskReport(DiskReport& report, Clock::time_point now)
{
    for (const auto& wholeFile : wholeFiles_) {
        if (wholeFile->transfer->Key() == report.key) {
            if (!wholeFile->transfer->Done()) {
                wholeFile->transfer->OnDiskReport(report, now);
            }
            if (phase_ == Phase::Closing) {
                MaybeFinishClosing();
            }
            return;
        }
    }
    if (auto* failed = std::get_if<disk::Failed>(&report.payload)) {
        Fail(std::move(failed->failure));
        return;
    }
    auto* closed = std::get_if<disk::Closed>(&report.payload);
    if (closed == nullptr) {
        return;  // a piece is closed as soon as it is received; its Durable reports add nothing
    }
    const auto open = openFiles_.find(report.key.file);
    if (open == openFiles_.end()) {
        return;
    }
    const size_t index = open->second;
    openFiles_.erase(open);
    Piece& piece = pieces_[index];
    if (piece.file == report.key.file && piece.state == PieceState::Closing) {
        if (closed->failure) {
            Fail(std::move(*closed->failure));
        } else {
            const auto mark = closed->watermarks.find(0);
            const std::int64_t durable = mark == closed->watermarks.end() ? 0 : mark->second;
            if (durable != piece.bytes) {
                Fail(MakeFailure(FailureReason::Internal, FailureClass::Fatal, "not every byte reached the disk"));
            } else {
                piece.state = PieceState::Durable;
                ++durablePieces_;
                const core::MediaTrackRequest& track = request_.tracks[piece.track];
                doneSinceReport_.push_back(core::MediaPieceDone{track.track, RequestOf(piece).index, piece.bytes});
            }
        }
    }
    if (phase_ == Phase::Closing) {
        MaybeFinishClosing();
    }
}

std::vector<core::MediaTrackRanges> MediaRun::WholeFileRanges() const
{
    std::vector<core::MediaTrackRanges> ranges;
    ranges.reserve(wholeFiles_.size());
    for (const auto& wholeFile : wholeFiles_) {
        ranges.push_back(core::MediaTrackRanges{wholeFile->track, wholeFile->ranges, wholeFile->totalSize});
    }
    return ranges;
}

void MediaRun::ReportProgress(Clock::time_point now)
{
    nextProgressAt_ = now + services_.tuning->progressInterval;

    std::int64_t received = receivedThisRun_;
    std::int64_t wholeFileSpeed = 0;
    int connections = static_cast<int>(connections_.size());
    for (const auto& wholeFile : wholeFiles_) {
        received += std::max<std::int64_t>(wholeFile->receivedBytes - wholeFile->startBytes, 0);
        wholeFileSpeed += wholeFile->bytesPerSecond;
        connections += wholeFile->connections;
    }
    constexpr auto kSpeedWindow = std::chrono::seconds(3);
    speedSamples_.emplace_back(now, receivedThisRun_);
    while (speedSamples_.size() > 2 && now - speedSamples_.front().first > kSpeedWindow) {
        speedSamples_.pop_front();
    }
    std::int64_t bytesPerSecond = 0;
    if (speedSamples_.size() >= 2) {
        const auto& [firstTime, firstBytes] = speedSamples_.front();
        const double seconds = std::chrono::duration<double>(now - firstTime).count();
        if (seconds > 0.0) {
            bytesPerSecond = static_cast<std::int64_t>(static_cast<double>(receivedThisRun_ - firstBytes) / seconds);
        }
    }

    core::MediaProgress progress;
    progress.done = std::move(doneSinceReport_);
    doneSinceReport_.clear();
    progress.ranges = WholeFileRanges();
    progress.receivedBytes = received;
    progress.bytesPerSecond = bytesPerSecond + wholeFileSpeed;
    progress.connections = connections;
    progress.lastRetry = lastRetry_;
    // As in Transfer::Report: cleared once nothing waits to retry. A whole-file track that still
    // waits reports its retry again with its next progress.
    const bool anyWaiting =
        std::any_of(pieces_.begin(), pieces_.end(), [](const Piece& piece) { return piece.retryAt.has_value(); }) ||
        std::any_of(keys_.begin(), keys_.end(), [](const auto& slot) { return slot.second.retryAt.has_value(); });
    if (!anyWaiting) {
        lastRetry_.reset();
    }
    services_.listener->OnEngineEvent(core::EngineEvent{request_.id, request_.epoch, std::move(progress)});
}

MediaRun::Clock::time_point MediaRun::NextDeadline(Clock::time_point now) const
{
    Clock::time_point deadline = now + std::chrono::hours(1);
    for (const auto& wholeFile : wholeFiles_) {
        if (!wholeFile->transfer->Done()) {
            deadline = std::min(deadline, wholeFile->transfer->NextDeadline(now));
        }
    }
    if (phase_ != Phase::Running) {
        return deadline;
    }
    if (closeRequested_) {
        return now;
    }
    deadline = std::min(deadline, nextProgressAt_);
    for (const size_t index : retrying_) {
        if (pieces_[index].retryAt) {
            deadline = std::min(deadline, *pieces_[index].retryAt);
        }
    }
    for (const auto& [slot, key] : keys_) {
        if (key.retryAt) {
            deadline = std::min(deadline, *key.retryAt);
        }
    }
    for (const auto& connection : connections_) {
        if (connection->pausedForMemory) {
            deadline = std::min(deadline, now + std::chrono::milliseconds(20));
        }
        if (connection->pausedForRate) {
            deadline = std::min(deadline, now + std::chrono::milliseconds(10));
        }
    }
    return deadline;
}

}  // namespace ldm::engine
