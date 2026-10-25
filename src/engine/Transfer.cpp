#include "engine/Transfer.h"

#include "engine/FailurePolicy.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace ldm::engine {

namespace {

using core::FailureClass;
using core::FailureReason;
using core::TransferFailure;

TransferFailure MakeFailure(FailureReason reason, FailureClass failureClass, std::string message)
{
    return TransferFailure{reason, failureClass, 0, std::move(message)};
}

bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix) noexcept
{
    if (text.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        const char a = (text[i] >= 'A' && text[i] <= 'Z') ? static_cast<char>(text[i] - 'A' + 'a') : text[i];
        if (a != prefix[i]) {
            return false;
        }
    }
    return true;
}

bool IsStrongEtag(const std::optional<std::string>& etag) noexcept
{
    return etag.has_value() && !etag->empty() && !etag->starts_with("W/");
}

// Content length that libcurl measured for the current response; for FTP this comes from SIZE.
std::optional<std::int64_t> CurlContentLength(CURL* easy)
{
    curl_off_t length = -1;
    if (curl_easy_getinfo(easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length) != CURLE_OK || length < 0) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(length);
}

std::string EffectiveUrl(CURL* easy, const std::string& fallback)
{
    char* url = nullptr;
    if (curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &url) == CURLE_OK && url != nullptr) {
        return url;
    }
    return fallback;
}

void AppendBytes(std::vector<std::byte>& target, const char* data, size_t size)
{
    const auto* bytes = reinterpret_cast<const std::byte*>(data);  // char and std::byte alias by definition
    target.insert(target.end(), bytes, bytes + size);
}

}  // namespace

Transfer::Transfer(const TransferServices& services, core::TransferRequest request, std::uint32_t file)
    : services_(services), request_(std::move(request)), file_(file)
{
    http_ = StartsWithIgnoreCase(request_.url, "http://") || StartsWithIgnoreCase(request_.url, "https://");
    finalUrl_ = request_.url;
}

Transfer::~Transfer()
{
    for (const auto& connection : connections_) {
        curl_multi_remove_handle(services_.multi, connection->easy.get());
        services_.pool->Release(static_cast<std::int64_t>(connection->pending.size()));
    }
}

bool Transfer::HasProgress() const noexcept
{
    return std::any_of(segments_.begin(), segments_.end(),
                       [](const Segment& segment) { return segment.position > segment.start; });
}

void Transfer::Begin(Clock::time_point now)
{
    bucket_.SetRate(request_.speedLimitBytesPerSecond, now);
    nextProgressAt_ = now + services_.tuning->progressInterval;

    for (const model::Segment& stored : request_.segments) {
        Segment segment;
        segment.key = nextSegmentKey_++;
        segment.start = stored.start;
        segment.end = stored.end;
        segment.position = stored.start + stored.done;
        segment.durableEnd = segment.position;
        segments_.push_back(std::move(segment));
    }
    std::sort(segments_.begin(), segments_.end(), [](const Segment& a, const Segment& b) { return a.start < b.start; });

    if (!HasProgress()) {
        // Fresh run: one segment, one plain GET without Range (checklist ENG-01).
        segments_.clear();
        Segment segment;
        segment.key = nextSegmentKey_++;
        segments_.push_back(std::move(segment));
        services_.disk->Open(Key(), request_.partialPath, OpenMode::CreateNew, 0);
        FillSlots(now);
        return;
    }

    resumeRun_ = true;
    totalSize_ = request_.knownSize;
    validators_ = request_.validators;
    contentType_ = request_.knownContentType;
    std::int64_t requiredLength = 0;
    for (const Segment& segment : segments_) {
        requiredLength = std::max(requiredLength, segment.durableEnd);
    }
    services_.disk->Open(Key(), request_.partialPath, OpenMode::Resume, requiredLength);

    const bool segmentsBounded = std::all_of(segments_.begin(), segments_.end(),
                                             [](const Segment& segment) { return segment.end.has_value(); });
    if (!totalSize_ || request_.resumable == false || !segmentsBounded) {
        Fail(MakeFailure(FailureReason::ResumeUnsupported, FailureClass::NeedsUser,
                         "the server does not support resuming this download"));
        return;
    }
    rangesPossible_ = true;
    resumable_ = true;
    if (std::all_of(segments_.begin(), segments_.end(), [](const Segment& segment) { return segment.Received(); })) {
        phase_ = Phase::Running;  // everything is already on disk; Service() completes the run
        return;
    }
    FillSlots(now);
}

void Transfer::RequestStop()
{
    if (phase_ == Phase::Closing || phase_ == Phase::Done) {
        return;
    }
    if (!closeRequested_) {
        closeRequested_ = core::TransferOutcome::Stopped;
    }
}

void Transfer::Fail(TransferFailure failure)
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

void Transfer::SetSpeedLimit(std::optional<std::int64_t> bytesPerSecond, Clock::time_point now)
{
    request_.speedLimitBytesPerSecond = bytesPerSecond;
    bucket_.SetRate(bytesPerSecond, now);
}

void Transfer::ExtendMedia(core::MediaExtension extension, Clock::time_point now)
{
    (void)extension;
    (void)now;
}

Transfer::Segment* Transfer::FindSegment(std::uint32_t key)
{
    const auto it = std::find_if(segments_.begin(), segments_.end(), [key](const Segment& s) { return s.key == key; });
    return it == segments_.end() ? nullptr : &*it;
}

Transfer::Segment* Transfer::SegmentStartingAt(std::int64_t offset)
{
    const auto it =
        std::find_if(segments_.begin(), segments_.end(), [offset](const Segment& s) { return s.start == offset; });
    return it == segments_.end() ? nullptr : &*it;
}

Transfer::Connection* Transfer::FindConnection(CURL* easy)
{
    const auto it = std::find_if(connections_.begin(), connections_.end(),
                                 [easy](const auto& connection) { return connection->easy.get() == easy; });
    return it == connections_.end() ? nullptr : it->get();
}

std::optional<std::string> Transfer::IfRangeValue() const
{
    if (IsStrongEtag(validators_.etag)) {
        return validators_.etag;
    }
    if (validators_.lastModified && !request_.ignoreLastModified) {
        return validators_.lastModified;
    }
    return std::nullopt;
}

bool Transfer::CanTake(Clock::time_point now) noexcept
{
    return bucket_.CanTake(now) && services_.globalBucket->CanTake(now) &&
           (services_.runBucket == nullptr || services_.runBucket->CanTake(now));
}

int Transfer::ConnectionLimit() const noexcept
{
    if (!rangesPossible_ || !totalSize_) {
        return 1;
    }
    return std::max(1, std::min(request_.maxConnections, allowedConnections_));
}

void Transfer::StartConnection(Segment& segment, bool probe, Clock::time_point now)
{
    (void)now;
    auto connection = std::make_unique<Connection>();
    connection->owner = this;
    connection->segmentKey = segment.key;
    connection->requestStart = segment.position;
    connection->position = segment.position;
    connection->probe = probe;
    // A capped run never asks for the whole file, so its first request is ranged too.
    connection->ranged = !(probe && !resumeRun_) || request_.maxRequestBytes.has_value();
    connection->easy.reset(curl_easy_init());
    if (!connection->easy) {
        Fail(MakeFailure(FailureReason::Internal, FailureClass::Fatal, "could not create a connection"));
        return;
    }
    CURL* easy = connection->easy.get();
    const TransferTuning& tuning = *services_.tuning;

    curl_easy_setopt(easy, CURLOPT_URL, request_.url.c_str());
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https,ftp,ftps");
    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https,ftp,ftps");
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_PIPEWAIT, 0L);
    curl_easy_setopt(easy, CURLOPT_BUFFERSIZE, 256L * 1024L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, tuning.connectTimeoutSeconds);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, tuning.stallTimeoutSeconds);
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, connection->errorBuffer.data());
    curl_easy_setopt(easy, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
    // "identity" is sent as Accept-Encoding (ENG-12), and a server that compresses anyway is still
    // decoded; such a response is then treated as unknown size without ranges.
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "identity");
    curl_easy_setopt(easy, CURLOPT_USERAGENT, request_.userAgent.c_str());
    if (request_.referer) {
        curl_easy_setopt(easy, CURLOPT_REFERER, request_.referer->c_str());
    }
    // An empty string disables proxy environment variables, so only LDM's settings apply.
    curl_easy_setopt(easy, CURLOPT_PROXY, request_.proxy ? request_.proxy->c_str() : "");
    if (request_.credentials) {
        curl_easy_setopt(easy, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_ANY));
        curl_easy_setopt(easy, CURLOPT_USERNAME, request_.credentials->user.c_str());
        curl_easy_setopt(easy, CURLOPT_PASSWORD, request_.credentials->password.c_str());
    }
    if (request_.ftpActiveMode) {
        curl_easy_setopt(easy, CURLOPT_FTPPORT, "-");
    }
    if (!request_.cookieLines.empty()) {
        curl_easy_setopt(easy, CURLOPT_COOKIEFILE, "");  // turns on the cookie engine without reading a file
        for (const std::string& line : request_.cookieLines) {
            curl_easy_setopt(easy, CURLOPT_COOKIELIST, line.c_str());  // copied by libcurl
        }
    }

    std::string range;
    if (connection->ranged) {
        range = std::to_string(segment.position) + "-";
        std::optional<std::int64_t> last;
        if (segment.end && totalSize_ && *segment.end < *totalSize_) {
            // Bounded ranges reach into the next segment so the overlap can be compared (ENG-03).
            last = std::min(*segment.end + tuning.overlapBytes, *totalSize_) - 1;
        }
        if (request_.maxRequestBytes && *request_.maxRequestBytes > 0 &&
            (!segment.end || segment.position + *request_.maxRequestBytes < *segment.end)) {
            // A part of the segment only; the next request continues where this one ends.
            last = segment.position + *request_.maxRequestBytes - 1;
            connection->cappedEnd = *last + 1;
        } else if (request_.maxRequestBytes && !last && segment.end) {
            last = *segment.end - 1;  // the file's tail, still asked for with both ends
        }
        if (last) {
            range += std::to_string(*last);
        }
        curl_easy_setopt(easy, CURLOPT_RANGE, range.c_str());  // copied by libcurl
    }
    curl_slist* headers = nullptr;
    if (connection->ranged && http_) {
        if (const auto ifRange = IfRangeValue()) {
            headers = curl_slist_append(headers, ("If-Range: " + *ifRange).c_str());
        }
    }
    if (probe && !resumeRun_ && http_ && request_.skipIfUnchanged && !request_.post) {
        // Synchronization asks the server whether its file is still the known one (RFC 9110 13.1).
        if (request_.validators.etag) {
            headers = curl_slist_append(headers, ("If-None-Match: " + *request_.validators.etag).c_str());
            conditionalRequest_ = true;
        } else if (request_.validators.lastModified && !request_.ignoreLastModified) {
            headers = curl_slist_append(headers, ("If-Modified-Since: " + *request_.validators.lastModified).c_str());
            conditionalRequest_ = true;
        }
    }
    if (request_.post) {
        // The form is sent again as the browser sent it. A redirect after it continues with GET, as
        // browsers do.
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request_.post->body.size()));
        curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, request_.post->body.data());
        headers = curl_slist_append(headers, ("Content-Type: " + request_.post->contentType).c_str());
    }
    for (const std::string& line : request_.headers) {
        headers = curl_slist_append(headers, line.c_str());
    }
    if (headers != nullptr) {
        connection->headerList.reset(headers);
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    }

    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &Transfer::WriteCallback);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, connection.get());
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, &Transfer::HeaderCallback);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, connection.get());
    ConnectionOwner::Attach(easy, this);
    connection->addressGuard.refuse = request_.refusePrivateAddresses && !request_.proxy;
    InstallAddressGuard(easy, &connection->addressGuard);

    if (curl_multi_add_handle(services_.multi, easy) != CURLM_OK) {
        Fail(MakeFailure(FailureReason::Internal, FailureClass::Fatal, "could not start a connection"));
        return;
    }
    segment.busy = true;
    segment.retryAt.reset();
    if (segment.position == segment.start) {
        segment.head.clear();
        segment.headFromStart = true;
    }
    connections_.push_back(std::move(connection));
}

void Transfer::RemoveConnection(Connection& connection)
{
    FlushPending(connection);
    curl_multi_remove_handle(services_.multi, connection.easy.get());
    if (Segment* segment = FindSegment(connection.segmentKey)) {
        segment->busy = false;
    }
    const auto it = std::find_if(connections_.begin(), connections_.end(),
                                 [&connection](const auto& candidate) { return candidate.get() == &connection; });
    if (it != connections_.end()) {
        connections_.erase(it);
    }
}

void Transfer::FlushPending(Connection& connection)
{
    if (connection.pending.empty()) {
        return;
    }
    const std::int64_t offset = connection.position - static_cast<std::int64_t>(connection.pending.size());
    services_.disk->Write(Key(), connection.segmentKey, offset, std::move(connection.pending));
    connection.pending = {};
}

size_t Transfer::WriteCallback(char* data, size_t size, size_t count, void* userdata)
{
    auto* connection = static_cast<Connection*>(userdata);
    return connection->owner->OnData(*connection, data, size * count);
}

size_t Transfer::HeaderCallback(char* data, size_t size, size_t count, void* userdata)
{
    auto* connection = static_cast<Connection*>(userdata);
    const size_t length = size * count;
    if (connection->owner->IsHttp()) {
        connection->headers.ApplyLine(std::string_view(data, length));
    }
    return length;
}

size_t Transfer::OnData(Connection& connection, const char* data, size_t size)
{
    if (phase_ == Phase::Closing || phase_ == Phase::Done || closeRequested_ || connection.reachedEnd) {
        return 0;
    }
    if (!connection.accepted && !AcceptResponse(connection)) {
        return 0;
    }
    const Clock::time_point now = Clock::now();
    if (!CanTake(now)) {
        connection.pausedForRate = true;
        return CURL_WRITEFUNC_PAUSE;
    }
    const auto chunk = static_cast<std::int64_t>(size);
    if (!services_.pool->TryReserve(chunk)) {
        connection.pausedForMemory = true;
        return CURL_WRITEFUNC_PAUSE;
    }
    bucket_.Take(chunk);
    services_.globalBucket->Take(chunk);
    if (services_.runBucket != nullptr) {
        services_.runBucket->Take(chunk);
    }

    Segment* segment = FindSegment(connection.segmentKey);
    assert(segment != nullptr);
    std::int64_t inRange = chunk;
    if (segment->end) {
        inRange = std::clamp<std::int64_t>(*segment->end - connection.position, 0, chunk);
    }
    if (inRange > 0) {
        const std::int64_t intoSegment = connection.position - segment->start;
        const std::int64_t overlap = services_.tuning->overlapBytes;
        if (segment->headFromStart && intoSegment < overlap &&
            intoSegment == static_cast<std::int64_t>(segment->head.size())) {
            AppendBytes(segment->head, data, static_cast<size_t>(std::min(overlap - intoSegment, inRange)));
            if (Segment* previous = SegmentEndingAt(segment->start)) {
                CheckOverlap(*previous, *segment);
            }
        }
        AppendBytes(connection.pending, data, static_cast<size_t>(inRange));
        connection.position += inRange;
        segment->position = connection.position;
        receivedThisRun_ += inRange;
    }
    // Only in-range bytes go to disk; the rest (overlap or excess) is released at once.
    services_.pool->Release(chunk - inRange);

    if (segment->end && connection.position >= *segment->end) {
        const std::int64_t wanted =
            totalSize_ ? std::min(services_.tuning->overlapBytes, std::max<std::int64_t>(*totalSize_ - *segment->end, 0)) : 0;
        const std::int64_t missing = wanted - static_cast<std::int64_t>(connection.overlap.size());
        const std::int64_t take = std::clamp<std::int64_t>(missing, 0, chunk - inRange);
        AppendBytes(connection.overlap, data + inRange, static_cast<size_t>(take));
        if (static_cast<std::int64_t>(connection.overlap.size()) >= wanted) {
            connection.reachedEnd = true;
            if (Segment* next = SegmentStartingAt(*segment->end); next != nullptr && wanted > 0) {
                next->overlapFromPrevious = connection.overlap;
                CheckOverlap(*segment, *next);
            }
        }
    }
    if (static_cast<std::int64_t>(connection.pending.size()) >= services_.tuning->writeChunkBytes) {
        FlushPending(connection);
    }
    return connection.reachedEnd ? 0 : size;
}

Transfer::Segment* Transfer::SegmentEndingAt(std::int64_t offset)
{
    const auto it = std::find_if(segments_.begin(), segments_.end(),
                                 [offset](const Segment& s) { return s.end.has_value() && *s.end == offset; });
    return it == segments_.end() ? nullptr : &*it;
}

void Transfer::CheckOverlap(Segment& previous, Segment& next)
{
    (void)previous;
    if (next.overlapFromPrevious.empty() || !next.headFromStart) {
        return;
    }
    const size_t compared = std::min(next.head.size(), next.overlapFromPrevious.size());
    if (!std::equal(next.head.begin(), next.head.begin() + static_cast<std::ptrdiff_t>(compared),
                    next.overlapFromPrevious.begin())) {
        Fail(MakeFailure(FailureReason::InconsistentContent, FailureClass::NeedsUser,
                         "the server sent different data for the same bytes on two connections"));
        return;
    }
    if (compared == next.overlapFromPrevious.size()) {
        next.overlapFromPrevious.clear();  // verified
    }
}

bool Transfer::AcceptResponse(Connection& connection)
{
    Segment* segment = FindSegment(connection.segmentKey);
    if (segment == nullptr) {
        return false;
    }
    if (http_) {
        const int status = connection.headers.status;
        if (status == 304 && connection.probe && conditionalRequest_) {
            unchangedOnServer_ = true;  // OnConnectionDone ends the run
            return false;
        }
        if (status == 200 && connection.ranged && !connection.probe) {
            connection.rangeRejected = true;
            return false;
        }
        if (auto failure = ClassifyHttpStatus(status)) {
            connection.failure = std::move(failure);
            return false;
        }
    }
    const bool accepted =
        connection.probe ? AcceptFirstResponse(connection, *segment) : AcceptExtraResponse(connection, *segment);
    connection.accepted = accepted;
    return accepted;
}

bool Transfer::AcceptFirstResponse(Connection& connection, Segment& segment)
{
    const ResponseHeaders& headers = connection.headers;
    CURL* easy = connection.easy.get();
    finalUrl_ = EffectiveUrl(easy, request_.url);
    const std::optional<std::string> contentType = http_ ? headers.contentType : std::nullopt;
    if (IsUnexpectedHtml(finalUrl_, contentType, request_.knownContentType)) {
        // IsUnexpectedHtml returns true only with a type; the detail names it, since the reason
        // already says what happened.
        connection.failure = MakeFailure(FailureReason::HtmlInsteadOfFile, FailureClass::Fatal,
                                         "Content-Type " + contentType->substr(0, 100));
        return false;
    }

    if (!resumeRun_) {
        std::optional<std::int64_t> size;
        bool rangesPossible = false;
        bool resumable = false;
        if (http_) {
            if (headers.status == 206 && !(headers.contentRange && headers.contentRange->total)) {
                // A part of the file without the file's length: its Content-Length is the part's,
                // and taking it as the whole would end the download early (a capped first request).
                connection.failure = MakeFailure(FailureReason::Protocol, FailureClass::Fatal,
                                                 "the server sent part of the file without saying how long the file is");
                return false;
            }
            const bool encoded = headers.HasContentEncoding();
            if (!encoded) {
                size = headers.status == 206 && headers.contentRange ? headers.contentRange->total : headers.contentLength;
            }
            rangesPossible = size.has_value() && !encoded && !headers.acceptRangesNone;
            resumable = rangesPossible && headers.acceptRanges;
            validators_ = model::Validators{headers.etag, headers.lastModified};
        } else {
            size = CurlContentLength(easy);
            rangesPossible = size.has_value();
            resumable = rangesPossible;
        }
        if (request_.skipIfUnchanged && !conditionalRequest_ && size && size == request_.knownSize) {
            // Nothing to ask the server with: the same size counts as the same file.
            unchangedOnServer_ = true;
            return false;
        }
        totalSize_ = size;
        rangesPossible_ = rangesPossible;
        resumable_ = resumable;
        segment.end = size;
        if (size) {
            services_.disk->SetLength(Key(), *size);
        }
    } else if (http_) {
        // Resuming: the answer must be the same file, served from the requested offset (ENG-07).
        const auto& range = headers.contentRange;
        bool changed = !range || range->first != connection.requestStart || range->total != totalSize_;
        if (IsStrongEtag(request_.validators.etag) && headers.etag && *headers.etag != *request_.validators.etag) {
            changed = true;
        }
        if (!request_.ignoreLastModified && request_.validators.lastModified && headers.lastModified &&
            *headers.lastModified != *request_.validators.lastModified) {
            changed = true;
        }
        if (changed) {
            connection.failure = MakeFailure(FailureReason::ResourceChanged, FailureClass::NeedsUser,
                                             "the file on the server has changed since the download started");
            return false;
        }
        validators_ = model::Validators{headers.etag ? headers.etag : request_.validators.etag,
                                        headers.lastModified ? headers.lastModified : request_.validators.lastModified};
    }
    if (contentType) {
        contentType_ = contentType;
    }
    if (http_ && headers.fileName) {
        fileName_ = headers.fileName;
    }
    responseInfoDirty_ = true;
    phase_ = Phase::Running;
    allowedConnections_ = 1 + services_.tuning->initialExtraConnections;
    nextRampAt_ = Clock::now() + services_.tuning->rampInterval;
    return true;
}

bool Transfer::AcceptExtraResponse(Connection& connection, Segment& segment)
{
    (void)segment;
    if (!http_) {
        return true;
    }
    const ResponseHeaders& headers = connection.headers;
    const auto& range = headers.contentRange;
    bool changed = !range || range->first != connection.requestStart || (range->total && range->total != totalSize_);
    if (IsStrongEtag(validators_.etag) && headers.etag && *headers.etag != *validators_.etag) {
        changed = true;
    }
    if (changed) {
        connection.failure = MakeFailure(FailureReason::ResourceChanged, FailureClass::NeedsUser,
                                         "the file on the server changed during the download");
        return false;
    }
    if (!resumable_) {
        resumable_ = true;  // a 206 proves the server serves ranges
        responseInfoDirty_ = true;
    }
    return true;
}

void Transfer::OnConnectionDone(CURL* easy, CURLcode result, Clock::time_point now)
{
    Connection* connection = FindConnection(easy);
    if (connection == nullptr) {
        return;
    }
    if (!connection->accepted && !connection->failure && !connection->rangeRejected && result == CURLE_OK &&
        phase_ != Phase::Closing && !closeRequested_) {
        // A response without a body (an error status, or an empty file) never reached OnData.
        AcceptResponse(*connection);
    }

    const std::uint32_t segmentKey = connection->segmentKey;
    // An FTP server that refuses REST on an extra connection is the FTP form of a server ignoring
    // ranges (ENG-05, ENG-13): fall back to the one connection that is already running.
    const bool restRefused = !http_ && result == CURLE_FTP_COULDNT_USE_REST && connection->ranged && !connection->probe;
    const bool rangeRejected = connection->rangeRejected || restRefused;
    const bool finishedNormally = connection->reachedEnd || (result == CURLE_OK && connection->accepted);
    std::optional<TransferFailure> failure = connection->failure;
    if (!failure && connection->addressGuard.refuse && connection->addressGuard.reachedPrivate) {
        failure = MakeFailure(FailureReason::PrivateAddress, FailureClass::Fatal,
                              "the address leads to a loopback or private network address");
    }
    if (!failure && !finishedNormally && !rangeRejected) {
        failure = ClassifyCurlResult(result, connection->errorBuffer.data());
    }
    const std::optional<int> retryAfter = connection->headers.retryAfterSeconds;
    const bool reachedEnd = connection->reachedEnd;
    const std::optional<std::int64_t> cappedEnd = connection->cappedEnd;
    const bool madeProgress = connection->position > connection->requestStart;
    RemoveConnection(*connection);  // `connection` is destroyed here

    if (phase_ == Phase::Closing || phase_ == Phase::Done || closeRequested_) {
        return;
    }
    if (unchangedOnServer_) {
        BeginClosing(core::TransferOutcome::Unchanged);
        return;
    }
    Segment* segment = FindSegment(segmentKey);
    if (segment == nullptr) {
        return;
    }

    if (rangeRejected) {
        // The server ignores ranges (ENG-05): give the split range back and keep one connection.
        rangesPossible_ = false;
        resumable_ = false;
        responseInfoDirty_ = true;
        Segment* previous = SegmentEndingAt(segment->start);
        if (previous == nullptr || segment->position != segment->start) {
            Fail(MakeFailure(FailureReason::ResumeUnsupported, FailureClass::NeedsUser,
                             "the server does not support resuming this download"));
            return;
        }
        previous->end = segment->end;
        const std::uint32_t key = segment->key;
        segments_.erase(std::remove_if(segments_.begin(), segments_.end(), [key](const Segment& s) { return s.key == key; }),
                        segments_.end());
        return;
    }
    if (finishedNormally) {
        if (!segment->end) {
            segment->end = segment->position;  // a stream without a length ends when the server closes it
            totalSize_ = segment->position;
            return;
        }
        if (segment->position >= *segment->end || reachedEnd) {
            segment->attempts = 0;
            return;
        }
        if (cappedEnd && segment->position >= *cappedEnd) {
            segment->attempts = 0;
            return;  // the part it asked for is complete; FillSlots serves the rest of the segment
        }
        failure = MakeFailure(FailureReason::ConnectionReset, FailureClass::Retryable,
                              "the server closed the connection before the segment was complete");
    }
    if (madeProgress) {
        // A connection that delivered data before failing is not a failed attempt in a row: a server
        // that drops every connection after a while must still be able to finish the file.
        segment->attempts = 0;
    }
    // A server that limits connections per client may refuse an extra one: BitChute's file servers
    // answer 404 past a few at once. While other connections of the run still receive, that says
    // nothing about the file, so the run keeps to the connections it has and asks for the segment
    // again later. A refusal of the only connection left is the server's answer, as before.
    const bool refused = failure && (failure->reason == FailureReason::NotFound || failure->reason == FailureReason::PermissionDenied);
    if (refused && phase_ == Phase::Running && !connections_.empty()) {
        connectionCeiling_ = static_cast<int>(connections_.size());
        allowedConnections_ = std::min(allowedConnections_, connectionCeiling_);
        failure->failureClass = FailureClass::Retryable;
    }
    HandleFailure(*segment, failure.value_or(MakeFailure(FailureReason::Internal, FailureClass::Fatal,
                                                          "connection ended without a result")),
                  retryAfter, now);
}

void Transfer::HandleFailure(Segment& segment, TransferFailure failure, std::optional<int> retryAfter,
                             Clock::time_point now)
{
    if (failure.failureClass != FailureClass::Retryable) {
        Fail(std::move(failure));
        return;
    }
    if (segment.position > segment.start && !rangesPossible_) {
        // Bytes already arrived and the server cannot continue from an offset (ENG-05).
        failure.reason = FailureReason::ResumeUnsupported;
        failure.failureClass = FailureClass::NeedsUser;
        failure.message = "the connection was interrupted and the server does not support resuming: " + failure.message;
        Fail(std::move(failure));
        return;
    }
    lastRetry_ = failure;
    segment.attempts += 1;
    if (segment.attempts >= request_.maxAttemptsPerSegment) {
        Fail(std::move(failure));
        return;
    }
    std::uniform_real_distribution<double> jitter(0.75, 1.25);
    segment.retryAt = now + RetryDelay(segment.attempts, jitter(*services_.random), retryAfter);
}

void Transfer::FillSlots(Clock::time_point now)
{
    if (closeRequested_ || phase_ == Phase::Closing || phase_ == Phase::Done) {
        return;
    }
    if (phase_ == Phase::Starting) {
        if (!connections_.empty()) {
            return;
        }
        // The first connection of the run validates the server's answer before any other opens.
        std::vector<core::PlanSegment> plan;
        for (const Segment& segment : segments_) {
            plan.push_back({segment.start, segment.end.value_or(segment.position + 1), segment.position, false});
        }
        const auto step = core::ChooseWork(plan, services_.tuning->planner);
        Segment& first = step ? segments_[step->segmentIndex] : segments_.front();
        if (first.retryAt && *first.retryAt > now) {
            return;
        }
        StartConnection(first, true, now);
        return;
    }

    if (allowedConnections_ < std::min(request_.maxConnections, connectionCeiling_) && now >= nextRampAt_) {
        allowedConnections_ += 1;
        nextRampAt_ = now + services_.tuning->rampInterval;
    }
    const int limit = ConnectionLimit();
    while (static_cast<int>(connections_.size()) < limit && !closeRequested_) {
        std::vector<core::PlanSegment> plan;
        plan.reserve(segments_.size());
        bool bounded = true;
        for (const Segment& segment : segments_) {
            if (!segment.end) {
                bounded = false;
                break;
            }
            const bool waiting = segment.retryAt && *segment.retryAt > now;
            plan.push_back({segment.start, *segment.end, segment.position, segment.busy || waiting});
        }
        if (!bounded) {
            // Unknown length: the single stream can only be retried from the start, which
            // HandleFailure already refused once bytes arrived.
            Segment& only = segments_.front();
            if (!only.busy && (!only.retryAt || *only.retryAt <= now) && only.position == only.start) {
                StartConnection(only, false, now);
            }
            return;
        }
        const auto step = core::ChooseWork(plan, services_.tuning->planner);
        if (!step) {
            return;
        }
        if (step->kind == core::PlanStep::Kind::Resume) {
            StartConnection(segments_[step->segmentIndex], false, now);
            continue;
        }
        Segment& split = segments_[step->segmentIndex];
        Segment added;
        added.key = nextSegmentKey_++;
        added.start = step->splitAt;
        added.end = split.end;
        added.position = step->splitAt;
        added.durableEnd = step->splitAt;
        split.end = step->splitAt;
        // Keep segments ordered by start; the new one follows the one it was split from.
        const auto insertAt = segments_.begin() + static_cast<std::ptrdiff_t>(step->segmentIndex) + 1;
        const auto inserted = segments_.insert(insertAt, std::move(added));
        StartConnection(*inserted, false, now);
    }
}

void Transfer::UnpauseConnections(Clock::time_point now)
{
    // Unpausing may call OnData synchronously; it only touches connection and segment fields,
    // never connections_ itself, so iterating by index stays valid.
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

void Transfer::Service(Clock::time_point now)
{
    if (phase_ == Phase::Closing || phase_ == Phase::Done) {
        return;
    }
    if (closeRequested_) {
        BeginClosing(*closeRequested_);
        return;
    }
    UnpauseConnections(now);
    if (closeRequested_) {
        BeginClosing(*closeRequested_);
        return;
    }
    if (responseInfoDirty_) {
        ReportResponseInfo();
    }
    MaybeComplete();
    if (phase_ == Phase::Closing) {
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

void Transfer::MaybeComplete()
{
    if (phase_ != Phase::Running || !connections_.empty()) {
        return;
    }
    const bool allReceived = std::all_of(segments_.begin(), segments_.end(), [](const Segment& s) { return s.Received(); });
    if (allReceived) {
        BeginClosing(core::TransferOutcome::Completed);
    }
}

void Transfer::BeginClosing(core::TransferOutcome outcome)
{
    while (!connections_.empty()) {
        RemoveConnection(*connections_.back());
    }
    closeRequested_.reset();
    closingOutcome_ = failure_ ? core::TransferOutcome::Failed : outcome;
    phase_ = Phase::Closing;
    services_.disk->Close(Key());
}

void Transfer::OnDiskReport(DiskReport& report, Clock::time_point now)
{
    (void)now;
    const auto applyWatermarks = [this](const Watermarks& watermarks) {
        for (Segment& segment : segments_) {
            const auto it = watermarks.find(segment.key);
            if (it != watermarks.end()) {
                segment.durableEnd = std::max(segment.durableEnd, it->second);
            }
        }
    };
    if (auto* durable = std::get_if<disk::Durable>(&report.payload)) {
        applyWatermarks(durable->watermarks);
        return;
    }
    if (auto* failed = std::get_if<disk::Failed>(&report.payload)) {
        Fail(std::move(failed->failure));
        return;
    }
    auto& closed = std::get<disk::Closed>(report.payload);
    if (phase_ != Phase::Closing) {
        return;
    }
    applyWatermarks(closed.watermarks);
    if (closed.failure) {
        if (!failure_) {
            failure_ = std::move(closed.failure);
        }
        closingOutcome_ = core::TransferOutcome::Failed;
    }
    if (closingOutcome_ == core::TransferOutcome::Completed) {
        const bool allDurable = std::all_of(segments_.begin(), segments_.end(), [](const Segment& s) {
            return s.end.has_value() && s.durableEnd >= *s.end;
        });
        if (!allDurable) {
            failure_ = MakeFailure(FailureReason::Internal, FailureClass::Fatal, "not every byte reached the disk");
            closingOutcome_ = core::TransferOutcome::Failed;
        }
    }
    if (responseInfoDirty_) {
        ReportResponseInfo();
    }
    core::TransferFinished finished;
    finished.outcome = closingOutcome_;
    finished.failure = closingOutcome_ == core::TransferOutcome::Failed ? failure_ : std::nullopt;
    finished.segments = DurableSegments();
    finished.totalSize = totalSize_;
    phase_ = Phase::Done;
    services_.listener->OnEngineEvent(core::EngineEvent{request_.id, request_.epoch, std::move(finished)});
}

std::vector<model::Segment> Transfer::DurableSegments() const
{
    std::vector<model::Segment> result;
    result.reserve(segments_.size());
    for (const Segment& segment : segments_) {
        std::int64_t done = std::max<std::int64_t>(segment.durableEnd - segment.start, 0);
        if (segment.end) {
            done = std::min(done, *segment.end - segment.start);
        }
        result.push_back(model::Segment{segment.start, segment.end, done});
    }
    std::sort(result.begin(), result.end(), [](const model::Segment& a, const model::Segment& b) { return a.start < b.start; });
    return result;
}

void Transfer::ReportResponseInfo()
{
    responseInfoDirty_ = false;
    core::ResponseInfo info;
    info.finalUrl = finalUrl_;
    info.totalSize = totalSize_;
    info.resumable = resumable_;
    info.validators = validators_;
    info.contentType = contentType_;
    info.fileName = fileName_;
    services_.listener->OnEngineEvent(core::EngineEvent{request_.id, request_.epoch, std::move(info)});
}

void Transfer::ReportProgress(Clock::time_point now)
{
    nextProgressAt_ = now + services_.tuning->progressInterval;

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

    core::ProgressInfo progress;
    progress.resumable = resumable_;
    progress.segments = DurableSegments();
    progress.totalSize = totalSize_;
    for (const Segment& segment : segments_) {
        progress.receivedBytes += segment.position - segment.start;
        if (segment.busy || segment.retryAt) {
            progress.connections.push_back(core::ConnectionView{segment.start, segment.end, segment.position - segment.start,
                                                                !segment.busy && segment.retryAt.has_value()});
        }
    }
    progress.bytesPerSecond = bytesPerSecond;
    progress.lastRetry = lastRetry_;
    // Reported at least once; once no segment waits to retry any more, the run has recovered and
    // the next report no longer says it is retrying.
    const bool anyWaiting = std::any_of(segments_.begin(), segments_.end(),
                                        [](const Segment& segment) { return !segment.busy && segment.retryAt; });
    if (!anyWaiting) {
        lastRetry_.reset();
    }
    services_.listener->OnEngineEvent(core::EngineEvent{request_.id, request_.epoch, std::move(progress)});
}

Transfer::Clock::time_point Transfer::NextDeadline(Clock::time_point now) const
{
    if (phase_ == Phase::Closing || phase_ == Phase::Done) {
        return now + std::chrono::hours(1);
    }
    if (closeRequested_ || responseInfoDirty_) {
        return now;
    }
    Clock::time_point deadline = nextProgressAt_;
    if (phase_ == Phase::Running && allowedConnections_ < std::min(request_.maxConnections, connectionCeiling_)) {
        deadline = std::min(deadline, nextRampAt_);
    }
    for (const Segment& segment : segments_) {
        if (segment.retryAt) {
            deadline = std::min(deadline, *segment.retryAt);
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
