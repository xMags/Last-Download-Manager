#pragma once

#include "core/TransferEngine.h"
#include "engine/Transfer.h"

#include <chrono>
#include <cstdint>
#include <memory>

namespace ldm::engine {

struct EngineOptions {
    std::int64_t bufferPoolBytes = 64LL * 1024 * 1024;
    std::chrono::milliseconds flushInterval = std::chrono::seconds(2);
    // How long the destructor waits for running transfers to acknowledge their stop.
    std::chrono::milliseconds shutdownTimeout = std::chrono::seconds(10);
    TransferTuning tuning;
};

// libcurl implementation of the transfer engine (design sections 3 and 4): one network thread
// running a curl multi loop for every connection, and one disk thread. Events go to `listener`
// from those threads.
class CurlEngine final : public core::TransferEngine {
public:
    // `listener` must outlive the engine.
    explicit CurlEngine(core::EngineListener& listener, EngineOptions options = {});
    // Stops every running transfer, waits up to the shutdown timeout for the acknowledgements,
    // then joins the network and disk threads.
    ~CurlEngine() override;

    CurlEngine(const CurlEngine&) = delete;
    CurlEngine& operator=(const CurlEngine&) = delete;

    void Start(core::TransferRequest request) override;
    void StartMedia(core::MediaRequest request) override;
    void Stop(model::DownloadId id, std::int64_t epoch) override;
    void SetSpeedLimit(model::DownloadId id, std::int64_t epoch, std::optional<std::int64_t> bytesPerSecond) override;
    void SetGlobalSpeedLimit(std::optional<std::int64_t> bytesPerSecond) override;
    void Finalize(core::FinalizeRequest request) override;
    void Fetch(core::FetchRequest request) override;

private:
    class Loop;
    std::unique_ptr<Loop> loop_;
};

}  // namespace ldm::engine
