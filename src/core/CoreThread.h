#pragma once

#include "base/Result.h"
#include "core/Commands.h"
#include "core/Coordinator.h"
#include "core/GrabberCommands.h"
#include "core/TransferEngine.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <variant>

namespace ldm::core {

// The core thread (design section 3): one queue of commands and engine events (proxy answers
// included), drained in order into the coordinator, plus the coordinator's timers. Commands from any thread are serialized
// here, so two simultaneous requests can never interleave. The Site Grabber's explorer, when
// there is one, runs here too: its commands, and the fetch answers and proxy lookups with Grabber
// ids (IsGrabberFetch), go to it instead of the coordinator.
//
// Lifetime: construct, create the engine with this object as its listener, create the
// coordinator, then Start(). Call Shutdown() before destroying the engine. Events the engine sends
// after Shutdown() are dropped.
class CoreThread final : public EngineListener, public CommandSink, public GrabberSink {
public:
    CoreThread() = default;
    ~CoreThread() override;

    CoreThread(const CoreThread&) = delete;
    CoreThread& operator=(const CoreThread&) = delete;

    // Starts the thread and runs coordinator.Initialize() on it, then grabber->Initialize().
    // Returns the coordinator's result; on failure the thread has already ended. Both must outlive
    // Shutdown().
    [[nodiscard]] base::Status Start(Coordinator& coordinator, GrabberComponent* grabber = nullptr);

    // Thread-safe. Ignored before Start() and after Shutdown().
    void Post(Command command) override;
    void Post(GrabberCommand command) override;
    void OnEngineEvent(EngineEvent event) override;

    // Stops every transfer, waits up to `timeout` for the engine to acknowledge, writes the final
    // checkpoint and joins the thread (design section 3, shutdown steps 2 and 4).
    void Shutdown(std::chrono::milliseconds timeout);

private:
    using Input = std::variant<Command, EngineEvent, GrabberCommand>;

    void Run(Coordinator& coordinator, GrabberComponent* grabber);

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Input> inbox_;
    bool accepting_ = false;
    bool shutdownRequested_ = false;
    std::chrono::milliseconds shutdownTimeout_{0};

    std::thread thread_;
};

}  // namespace ldm::core
