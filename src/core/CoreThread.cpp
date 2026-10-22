#include "core/CoreThread.h"

#include <future>
#include <optional>
#include <utility>

namespace ldm::core {

CoreThread::~CoreThread()
{
    // Normal shutdown goes through Shutdown(); this only guarantees the thread never outlives us.
    Shutdown(std::chrono::milliseconds(0));
}

base::Status CoreThread::Start(Coordinator& coordinator)
{
    std::promise<base::Status> initialized;
    std::future<base::Status> result = initialized.get_future();
    {
        std::lock_guard lock(mutex_);
        accepting_ = true;
    }
    thread_ = std::thread([this, &coordinator, &initialized] {
        base::Status status = coordinator.Initialize();
        const bool ok = status.Ok();
        initialized.set_value(std::move(status));  // `initialized` is not touched after this line
        if (ok) {
            Run(coordinator);
        }
    });
    base::Status status = result.get();
    if (!status) {
        thread_.join();
        std::lock_guard lock(mutex_);
        accepting_ = false;
        inbox_.clear();
    }
    return status;
}

void CoreThread::Post(Command command)
{
    {
        std::lock_guard lock(mutex_);
        if (!accepting_ || shutdownRequested_) {
            return;
        }
        inbox_.emplace_back(std::move(command));
    }
    wake_.notify_one();
}

void CoreThread::OnEngineEvent(EngineEvent event)
{
    {
        std::lock_guard lock(mutex_);
        if (!accepting_) {
            return;
        }
        inbox_.emplace_back(std::move(event));
    }
    wake_.notify_one();
}

void CoreThread::Shutdown(std::chrono::milliseconds timeout)
{
    {
        std::lock_guard lock(mutex_);
        if (!thread_.joinable()) {
            return;
        }
        if (!shutdownRequested_) {
            shutdownRequested_ = true;
            shutdownTimeout_ = timeout;
        }
    }
    wake_.notify_one();
    thread_.join();
    std::lock_guard lock(mutex_);
    accepting_ = false;
    inbox_.clear();
}

void CoreThread::Run(Coordinator& coordinator)
{
    using Clock = std::chrono::steady_clock;
    std::optional<Clock::time_point> shutdownDeadline;
    std::deque<Input> batch;
    while (true) {
        bool shutdownRequested = false;
        {
            std::unique_lock lock(mutex_);
            auto deadline = coordinator.NextDeadline();
            if (shutdownDeadline) {
                deadline = std::min(deadline, *shutdownDeadline);
            }
            const auto ready = [this] { return !inbox_.empty() || shutdownRequested_; };
            if (deadline == Clock::time_point::max()) {
                wake_.wait(lock, ready);
            } else {
                wake_.wait_until(lock, deadline, ready);
            }
            batch.swap(inbox_);
            shutdownRequested = shutdownRequested_;
            if (shutdownRequested && !shutdownDeadline) {
                shutdownDeadline = Clock::now() + shutdownTimeout_;
            }
        }
        for (Input& input : batch) {
            if (auto* command = std::get_if<Command>(&input)) {
                coordinator.Handle(std::move(*command));
            } else {
                coordinator.OnEngineEvent(std::move(std::get<EngineEvent>(input)));
            }
        }
        batch.clear();
        if (shutdownRequested) {
            // Idempotent: stops whatever is running, including runs started by commands that were
            // queued before the shutdown request. Engine acknowledgements keep arriving above.
            coordinator.BeginShutdown();
            if (coordinator.ShutdownComplete() || Clock::now() >= *shutdownDeadline) {
                // Past the deadline the last durable checkpoints are still safe (design section 5).
                coordinator.FinalCheckpoint();
                coordinator.Tick();
                return;
            }
        }
        coordinator.Tick();
    }
}

}  // namespace ldm::core
