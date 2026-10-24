#pragma once

#include "core/LaunchRequest.h"

#include <windows.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace ldm::platform {

// Single instance and command forwarding (checklist W-05, A-10; design section 7).
//
// Protocol: one request per connection. Each message is a 4-byte little-endian length followed
// by that many bytes of UTF-8 JSON, at most kMaxMessageBytes. Requests carry the protocol version
// and a request id; the answer echoes the id. The pipe accepts only the current user (owner-only
// DACL) and only local clients.
inline constexpr std::uint32_t kMaxMessageBytes = 1024 * 1024;
inline constexpr int kProtocolVersion = 1;

[[nodiscard]] std::string EncodeLaunchRequest(const core::LaunchRequest& request, std::string_view requestId);
// Validates every field as untrusted input; nothing when anything is missing or wrong.
[[nodiscard]] std::optional<core::LaunchRequest> DecodeLaunchRequest(std::string_view json);

// Held by the first instance for its lifetime.
class InstanceLock {
public:
    // Nothing when another instance of this user in this session holds the lock.
    [[nodiscard]] static std::unique_ptr<InstanceLock> TryAcquire();
    ~InstanceLock();
    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;

private:
    InstanceLock(HANDLE mutex, HANDLE running) noexcept : mutex_(mutex), running_(running) {}
    HANDLE mutex_ = nullptr;
    HANDLE running_ = nullptr;  // kRunningMutexName, for the uninstaller; may be null
};

// Exists while LDM runs in this session. Its name is fixed, unlike the instance lock's, so the
// uninstaller (installer/ldm.iss.in) can ask the user to exit LDM before removing its files.
inline constexpr wchar_t kRunningMutexName[] = L"Local\\LastDownloadManager.Running";

// Serves the pipe on its own thread. `onRequest` runs on that thread and must only post.
class InstanceServer {
public:
    explicit InstanceServer(std::function<void(core::LaunchRequest)> onRequest);
    // Stops accepting and joins the thread.
    ~InstanceServer();
    InstanceServer(const InstanceServer&) = delete;
    InstanceServer& operator=(const InstanceServer&) = delete;

    // False when the pipe could not be created (for example, another process already owns it).
    [[nodiscard]] bool Start();

private:
    void Run();

    std::function<void(core::LaunchRequest)> onRequest_;
    HANDLE stopEvent_ = nullptr;
    HANDLE firstPipe_ = INVALID_HANDLE_VALUE;
    std::thread thread_;
};

// Sends a request to the running instance and waits for its answer. False when no instance
// accepted it within a few seconds.
[[nodiscard]] bool ForwardToRunningInstance(const core::LaunchRequest& request);

// Lets the running instance take the foreground when the second launch asks it to show itself.
void AllowRunningInstanceToActivate();

}  // namespace ldm::platform
