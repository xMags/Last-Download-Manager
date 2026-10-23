#pragma once

#include "core/Ports.h"
#include "core/TransferEngine.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace ldm::platform {

// Options > Proxy (checklist O-06). libcurl cannot run PAC scripts, so "system" and "automatic
// configuration script" modes ask WinHTTP for the proxy of each address and hand the answer to
// libcurl (design section 4). Manual mode builds the proxy URL from the settings, including
// DPAPI-protected credentials and the bypass list.
//
// WinHTTP may fetch a PAC script or run WPAD detection, which can take seconds. Those lookups run
// on this object's own thread and answer through `listener` as a ProxyResolved event; everything
// else (no proxy, manual proxies, a static system proxy) answers at once.
//
// Threads: Configure() and Resolve() are called on the core thread; lookups run on the worker,
// which owns the WinHTTP session and reads only the job it was given.
class WinHttpProxyResolver final : public core::ProxyResolver {
public:
    // `listener` must outlive this object; it receives answers on the worker thread.
    WinHttpProxyResolver(core::SecretProtector& secrets, core::EngineListener& listener);
    // Stops the worker, waiting for a lookup in progress (each is bounded by WinHTTP timeouts).
    ~WinHttpProxyResolver() override;
    WinHttpProxyResolver(const WinHttpProxyResolver&) = delete;
    WinHttpProxyResolver& operator=(const WinHttpProxyResolver&) = delete;

    void Configure(const model::Settings& settings) override;
    core::ProxyLookup Resolve(model::DownloadId id, std::int64_t epoch, const std::string& url) override;

private:
    // A lookup for the worker, with everything it needs copied in.
    struct Job {
        model::DownloadId id;
        std::int64_t epoch = 0;
        std::string url;
        std::optional<std::string> script;      // PAC address; nothing with autoDetect
        bool autoDetect = false;                // WPAD
        std::optional<std::string> staticProxy;  // fallback when detection or the script fails
        std::string bypass;
    };

    void RunWorker();
    [[nodiscard]] std::optional<std::string> LookUp(const Job& job);
    [[nodiscard]] std::optional<std::string> ManualProxy(const std::string& url) const;
    [[nodiscard]] bool Bypassed(const std::string& host) const;

    core::SecretProtector& secrets_;
    core::EngineListener& listener_;
    model::Settings settings_;  // core thread only

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    bool stopping_ = false;
    void* session_ = nullptr;  // HINTERNET, worker thread only, opened on first use
    std::thread worker_;       // started on the first lookup that needs it
};

}  // namespace ldm::platform
