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
};

// Random version 4 UUIDs from the system's cryptographic generator.
class RandomIdGenerator final : public core::IdGenerator {
public:
    std::string NewPublicId() override;
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
