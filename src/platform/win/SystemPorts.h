#pragma once

#include "core/Ports.h"

#include <windows.h>

namespace ldm::platform {

class SystemClock final : public core::Clock {
public:
    std::int64_t NowUnixMs() override;
    std::chrono::steady_clock::time_point Steady() override;
};

class WinFileSystem final : public core::FileSystem {
public:
    bool Exists(const std::filesystem::path& path) override;
    std::optional<std::int64_t> FileSize(const std::filesystem::path& path) override;
    base::Status Remove(const std::filesystem::path& path) override;
    base::Status Recycle(const std::filesystem::path& path) override;
    base::Status CreateDirectories(const std::filesystem::path& path) override;
    base::Status RemoveFolder(const std::filesystem::path& path) override;
    base::Status WriteFile(const std::filesystem::path& path, std::string_view bytes, bool replace) override;
    base::Result<std::string> ReadFile(const std::filesystem::path& path, std::size_t maxBytes) override;
};

// Random version 4 UUIDs from the system's cryptographic generator.
class RandomIdGenerator final : public core::IdGenerator {
public:
    std::string NewPublicId() override;
};

// The time zone Windows is set to at each call, with its daylight saving rules, so a change of
// zone while LDM runs applies at once.
class WinTimeZone final : public core::TimeZone {
public:
    std::int64_t LocalToUnixMs(std::int64_t localMs) const override;
    std::int64_t UnixToLocalMs(std::int64_t unixMs) const override;
};

// A waitable timer that resumes the computer from sleep when it fires; Windows' power options
// decide whether wake timers may do that. Nothing waits on it: waking up is all it does, and the
// main window then tells the core to look at its schedules.
class WaitableWakeTimer final : public core::WakeTimer {
public:
    WaitableWakeTimer();
    ~WaitableWakeTimer() override;
    WaitableWakeTimer(const WaitableWakeTimer&) = delete;
    WaitableWakeTimer& operator=(const WaitableWakeTimer&) = delete;

    void Arm(std::int64_t unixMs) override;
    void Disarm() override;

private:
    HANDLE timer_ = nullptr;
};

// A "system required" power request while transfers run, so Windows does not sleep mid-download
// (checklist ENG-17). The display may still turn off.
class PowerRequest final : public core::PowerRequests {
public:
    PowerRequest();
    ~PowerRequest() override;
    PowerRequest(const PowerRequest&) = delete;
    PowerRequest& operator=(const PowerRequest&) = delete;

    void SetTransfersActive(bool active) override;

private:
    HANDLE request_ = INVALID_HANDLE_VALUE;
    bool active_ = false;
};

}  // namespace ldm::platform
