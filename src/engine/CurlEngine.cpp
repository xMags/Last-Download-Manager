#include "engine/CurlEngine.h"

#include "engine/BufferPool.h"
#include "engine/CurlHandles.h"
#include "engine/DiskWriter.h"
#include "engine/MemoryFetch.h"
#include "engine/TokenBucket.h"

#include <curl/curl.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace ldm::engine {

namespace {

using Clock = std::chrono::steady_clock;

// libcurl's process-wide state is reference counted inside libcurl, so each engine holds one count.
class CurlGlobal {
public:
    CurlGlobal()
    {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw std::runtime_error("libcurl initialization failed");
        }
    }
    ~CurlGlobal() { curl_global_cleanup(); }
    CurlGlobal(const CurlGlobal&) = delete;
    CurlGlobal& operator=(const CurlGlobal&) = delete;
};

struct StartCommand {
    core::TransferRequest request;
};
struct StopCommand {
    model::DownloadId id;
    std::int64_t epoch;
};
struct SpeedCommand {
    model::DownloadId id;
    std::int64_t epoch;
    std::optional<std::int64_t> bytesPerSecond;
};
struct GlobalSpeedCommand {
    std::optional<std::int64_t> bytesPerSecond;
};
struct ShutdownCommand {};
struct FetchCommand {
    core::FetchRequest request;
};

using Command =
    std::variant<StartCommand, StopCommand, SpeedCommand, GlobalSpeedCommand, ShutdownCommand, DiskReport, FetchCommand>;

}  // namespace

class CurlEngine::Loop {
public:
    Loop(core::EngineListener& listener, EngineOptions options)
        : listener_(listener),
          options_(std::move(options)),
          multi_(curl_multi_init()),
          pool_(options_.bufferPoolBytes, [this] { Wake(); }),
          random_(std::random_device{}())
    {
        if (!multi_) {
            throw std::runtime_error("libcurl multi handle could not be created");
        }
        // One TCP connection per segment; HTTP/2 itself stays in use per connection.
        curl_multi_setopt(multi_.get(), CURLMOPT_PIPELINING, static_cast<long>(CURLPIPE_NOTHING));
        disk_ = std::make_unique<DiskWriter>(
            pool_, [this](DiskReport report) { Post(std::move(report)); },
            [this](core::EngineEvent event) { listener_.OnEngineEvent(std::move(event)); }, options_.flushInterval);
        services_.multi = multi_.get();
        services_.disk = disk_.get();
        services_.pool = &pool_;
        services_.globalBucket = &globalBucket_;
        services_.listener = &listener_;
        services_.random = &random_;
        services_.tuning = &options_.tuning;
        thread_ = std::jthread([this] { Run(); });
    }

    ~Loop()
    {
        Post(ShutdownCommand{});
        if (thread_.joinable()) {
            thread_.join();
        }
        // Transfers that missed the shutdown deadline give up their handles and buffers; their last
        // durable checkpoints are already with the core. The disk writer then closes their files.
        transfers_.clear();
        fetches_.clear();
        disk_.reset();
    }

    Loop(const Loop&) = delete;
    Loop& operator=(const Loop&) = delete;

    void Post(Command command)
    {
        {
            std::lock_guard lock(mutex_);
            inbox_.push_back(std::move(command));
        }
        Wake();
    }

    void Finalize(core::FinalizeRequest request) { disk_->Finalize(std::move(request)); }

private:
    void Wake() { curl_multi_wakeup(multi_.get()); }

    void Run()
    {
        std::optional<Clock::time_point> shutdownDeadline;
        std::vector<Command> batch;
        while (true) {
            {
                std::lock_guard lock(mutex_);
                batch.swap(inbox_);
            }
            Clock::time_point now = Clock::now();
            for (Command& command : batch) {
                Dispatch(command, now, shutdownDeadline);
            }
            batch.clear();
            if (shutdownDeadline && (transfers_.empty() || now >= *shutdownDeadline)) {
                // Fetches still running are answered, so each request gets its one answer.
                while (!fetches_.empty()) {
                    FinishFetch(fetches_.begin(), CURLE_ABORTED_BY_CALLBACK);
                }
                return;
            }

            int running = 0;
            curl_multi_perform(multi_.get(), &running);
            int queued = 0;
            while (CURLMsg* message = curl_multi_info_read(multi_.get(), &queued)) {
                if (message->msg != CURLMSG_DONE) {
                    continue;
                }
                CURL* easy = message->easy_handle;
                const CURLcode result = message->data.result;
                if (const auto fetch = fetches_.find(easy); fetch != fetches_.end()) {
                    FinishFetch(fetch, result);
                    continue;
                }
                if (Transfer* owner = Transfer::OwnerOf(easy)) {
                    owner->OnConnectionDone(easy, result, Clock::now());
                }
            }

            now = Clock::now();
            Clock::time_point deadline = now + std::chrono::seconds(1);
            // Downloads take turns being serviced first, so under the global limit the one with
            // the lowest id does not take every refill (Q-09).
            if (!transfers_.empty()) {
                std::vector<Transfer*> order;
                order.reserve(transfers_.size());
                for (auto& [id, transfer] : transfers_) {
                    order.push_back(transfer.get());
                }
                const size_t first = serviceRotation_++ % order.size();
                for (size_t step = 0; step < order.size(); ++step) {
                    order[(first + step) % order.size()]->Service(now);
                }
            }
            for (auto it = transfers_.begin(); it != transfers_.end();) {
                if (it->second->Done()) {
                    it = transfers_.erase(it);
                    continue;
                }
                deadline = std::min(deadline, it->second->NextDeadline(now));
                ++it;
            }

            long curlTimeout = -1;
            curl_multi_timeout(multi_.get(), &curlTimeout);
            auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            if (curlTimeout >= 0) {
                wait = std::min(wait, std::chrono::milliseconds(curlTimeout));
            }
            wait = std::clamp(wait, std::chrono::milliseconds(0), std::chrono::milliseconds(1000));
            curl_multi_poll(multi_.get(), nullptr, 0, static_cast<int>(wait.count()), nullptr);
        }
    }

    using Fetches = std::map<CURL*, std::unique_ptr<MemoryFetch>>;

    void FinishFetch(Fetches::iterator fetch, CURLcode result)
    {
        core::FetchFinished finished = fetch->second->Finish(result);
        fetches_.erase(fetch);  // removes the handle from the multi handle
        listener_.OnEngineEvent(core::EngineEvent{model::DownloadId{}, 0, std::move(finished)});
    }

    void StartFetch(core::FetchRequest request, bool shuttingDown)
    {
        if (shuttingDown) {
            core::FetchFinished refused;
            refused.fetchId = request.fetchId;
            refused.failure = core::TransferFailure{core::FailureReason::Internal, core::FailureClass::Fatal, 0,
                                                    "the engine is shutting down"};
            listener_.OnEngineEvent(core::EngineEvent{model::DownloadId{}, 0, std::move(refused)});
            return;
        }
        auto fetch = std::make_unique<MemoryFetch>(multi_.get(), std::move(request), options_.tuning.connectTimeoutSeconds,
                                                   options_.tuning.stallTimeoutSeconds);
        if (!fetch->Started()) {
            listener_.OnEngineEvent(core::EngineEvent{model::DownloadId{}, 0, fetch->Finish(CURLE_FAILED_INIT)});
            return;
        }
        CURL* handle = fetch->Handle();
        fetches_.emplace(handle, std::move(fetch));
    }

    Transfer* FindRun(model::DownloadId id, std::int64_t epoch)
    {
        const auto it = transfers_.find(id);
        if (it == transfers_.end() || it->second->Key().epoch != epoch) {
            return nullptr;
        }
        return it->second.get();
    }

    void Dispatch(Command& command, Clock::time_point now, std::optional<Clock::time_point>& shutdownDeadline)
    {
        if (auto* start = std::get_if<StartCommand>(&command)) {
            core::TransferRequest& request = start->request;
            if (shutdownDeadline || transfers_.contains(request.id)) {
                // Refused without touching the file: report the stored progress back unchanged.
                core::TransferFinished finished;
                finished.outcome = core::TransferOutcome::Failed;
                finished.failure = core::TransferFailure{core::FailureReason::Internal, core::FailureClass::Fatal, 0,
                                                         shutdownDeadline ? "the engine is shutting down"
                                                                          : "the download is already running"};
                finished.segments = request.segments;
                finished.totalSize = request.knownSize;
                listener_.OnEngineEvent(core::EngineEvent{request.id, request.epoch, std::move(finished)});
                return;
            }
            const model::DownloadId id = request.id;
            auto transfer = std::make_unique<Transfer>(services_, std::move(request));
            Transfer& started = *transfer;
            transfers_.emplace(id, std::move(transfer));
            started.Begin(now);
            return;
        }
        if (auto* stop = std::get_if<StopCommand>(&command)) {
            if (Transfer* transfer = FindRun(stop->id, stop->epoch)) {
                transfer->RequestStop();
            }
            return;
        }
        if (auto* speed = std::get_if<SpeedCommand>(&command)) {
            if (Transfer* transfer = FindRun(speed->id, speed->epoch)) {
                transfer->SetSpeedLimit(speed->bytesPerSecond, now);
            }
            return;
        }
        if (auto* global = std::get_if<GlobalSpeedCommand>(&command)) {
            globalBucket_.SetRate(global->bytesPerSecond, now);
            return;
        }
        if (auto* fetch = std::get_if<FetchCommand>(&command)) {
            StartFetch(std::move(fetch->request), shutdownDeadline.has_value());
            return;
        }
        if (std::holds_alternative<ShutdownCommand>(command)) {
            if (!shutdownDeadline) {
                shutdownDeadline = now + options_.shutdownTimeout;
                for (auto& [id, transfer] : transfers_) {
                    transfer->RequestStop();
                }
            }
            return;
        }
        auto& report = std::get<DiskReport>(command);
        if (Transfer* transfer = FindRun(report.key.id, report.key.epoch)) {
            transfer->OnDiskReport(report, now);
        }
    }

    // Declaration order is construction order; the destructor tears down explicitly.
    CurlGlobal curlGlobal_;
    core::EngineListener& listener_;
    EngineOptions options_;
    CurlMulti multi_;
    BufferPool pool_;
    TokenBucket globalBucket_;
    std::mt19937 random_;
    std::unique_ptr<DiskWriter> disk_;
    TransferServices services_;

    std::mutex mutex_;
    std::vector<Command> inbox_;

    // Network thread only.
    std::map<model::DownloadId, std::unique_ptr<Transfer>> transfers_;
    Fetches fetches_;
    size_t serviceRotation_ = 0;  // see the service loop

    std::jthread thread_;
};

CurlEngine::CurlEngine(core::EngineListener& listener, EngineOptions options)
    : loop_(std::make_unique<Loop>(listener, std::move(options)))
{
}

CurlEngine::~CurlEngine() = default;

void CurlEngine::Start(core::TransferRequest request)
{
    loop_->Post(StartCommand{std::move(request)});
}

void CurlEngine::Stop(model::DownloadId id, std::int64_t epoch)
{
    loop_->Post(StopCommand{id, epoch});
}

void CurlEngine::SetSpeedLimit(model::DownloadId id, std::int64_t epoch, std::optional<std::int64_t> bytesPerSecond)
{
    loop_->Post(SpeedCommand{id, epoch, bytesPerSecond});
}

void CurlEngine::SetGlobalSpeedLimit(std::optional<std::int64_t> bytesPerSecond)
{
    loop_->Post(GlobalSpeedCommand{bytesPerSecond});
}

void CurlEngine::Finalize(core::FinalizeRequest request)
{
    loop_->Finalize(std::move(request));
}

void CurlEngine::Fetch(core::FetchRequest request)
{
    loop_->Post(FetchCommand{std::move(request)});
}

}  // namespace ldm::engine
