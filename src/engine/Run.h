#pragma once

#include "engine/DiskWriter.h"

#include <curl/curl.h>

#include <chrono>
#include <cstdint>
#include <optional>

namespace ldm::engine {

// One run of one download as the network loop drives it: a file (Transfer) or a stream
// (MediaRun). Every method runs on the network thread.
class Run {
public:
    using Clock = std::chrono::steady_clock;

    virtual ~Run() = default;

    // The download and epoch of the run; `file` is 0 here, a run may own several files.
    [[nodiscard]] virtual RunKey Key() const noexcept = 0;
    virtual void RequestStop() = 0;
    virtual void SetSpeedLimit(std::optional<std::int64_t> bytesPerSecond, Clock::time_point now) = 0;
    // A report about one of the run's files, with the run's id and epoch.
    virtual void OnDiskReport(DiskReport& report, Clock::time_point now) = 0;
    // Opens connections, retries, unpauses, reports progress and finishes. Called after every
    // curl_multi_perform and whenever NextDeadline() passes.
    virtual void Service(Clock::time_point now) = 0;
    [[nodiscard]] virtual Clock::time_point NextDeadline(Clock::time_point now) const = 0;
    // True once the run's last event was sent; the loop then destroys it.
    [[nodiscard]] virtual bool Done() const noexcept = 0;
};

// What CURLOPT_PRIVATE of every easy handle a run adds points at: the object to tell when the
// handle is done. The loop routes CURLMSG_DONE through it.
class ConnectionOwner {
public:
    virtual void OnConnectionDone(CURL* easy, CURLcode result, Run::Clock::time_point now) = 0;

    // Sets `owner` as the handle's CURLOPT_PRIVATE.
    static void Attach(CURL* easy, ConnectionOwner* owner);
    [[nodiscard]] static ConnectionOwner* Of(CURL* easy);

protected:
    ~ConnectionOwner() = default;
};

}  // namespace ldm::engine
