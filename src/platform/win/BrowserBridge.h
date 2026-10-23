#pragma once

#include "ui/BrowserLink.h"

#include <windows.h>

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ldm::platform {

// Serves the browser pipe that ldm-host.exe connects to (M3 design section 3): up to
// kMaxConnections persistent connections, framed JSON in both directions, owner-only access,
// local clients only. Each connection must start with the host's "host" message; anything that
// fails validation closes that connection.
//
// Threading: one thread owns every pipe instance. `onEvent` runs on that thread and must only
// post. Send() may be called from any thread.
class BrowserBridge final : public ui::BrowserLink {
public:
    static constexpr int kMaxConnections = 8;

    explicit BrowserBridge(std::function<void(ui::BrowserEvent)> onEvent);
    ~BrowserBridge() override;
    BrowserBridge(const BrowserBridge&) = delete;
    BrowserBridge& operator=(const BrowserBridge&) = delete;

    // False when the pipe could not be created (another process owns the name, or Windows refused).
    [[nodiscard]] bool Start();
    // Closes every connection and joins the thread. Called before whatever `onEvent` posts to goes
    // away; the destructor calls it too.
    void Stop();

    void Send(ui::BrowserConnectionId connection, std::string json) override;

private:
    struct Slot;

    void Run();
    void Listen(Slot& slot);
    [[nodiscard]] bool BeginRead(Slot& slot);
    [[nodiscard]] bool Advance(Slot& slot);
    [[nodiscard]] bool Deliver(Slot& slot);
    // Ends the connection; `notify` reports it to onEvent_ when the host had introduced itself.
    void Close(Slot& slot, bool notify);
    void FlushOutbox();

    std::function<void(ui::BrowserEvent)> onEvent_;
    HANDLE stopEvent_ = nullptr;
    HANDLE wakeEvent_ = nullptr;  // set when the outbox has messages
    std::vector<std::unique_ptr<Slot>> slots_;  // owned by the bridge thread while it runs
    ui::BrowserConnectionId lastConnection_ = 0;
    std::thread thread_;

    std::mutex mutex_;  // guards outbox_
    std::deque<std::pair<ui::BrowserConnectionId, std::string>> outbox_;
};

}  // namespace ldm::platform
