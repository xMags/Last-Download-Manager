#pragma once

#include "base/Result.h"
#include "model/Ids.h"
#include "model/Settings.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Narrow interfaces the coordinator uses to reach the operating system. Implementations live in
// platform/win (production) and in the tests (fakes). Every port is called on the core thread only.
namespace ldm::core {

class Clock {
public:
    virtual ~Clock() = default;
    // Wall-clock time for stored timestamps (created, last try, completed): Unix milliseconds, UTC.
    [[nodiscard]] virtual std::int64_t NowUnixMs() = 0;
    // Monotonic time for timers.
    [[nodiscard]] virtual std::chrono::steady_clock::time_point Steady() = 0;
};

// The few synchronous file operations the core performs itself. Transfers and the final move go
// through the engine's disk thread instead.
class FileSystem {
public:
    virtual ~FileSystem() = default;
    [[nodiscard]] virtual bool Exists(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual std::optional<std::int64_t> FileSize(const std::filesystem::path& path) = 0;
    // Deletes a file. A file that does not exist counts as deleted.
    [[nodiscard]] virtual base::Status Remove(const std::filesystem::path& path) = 0;
    // Moves a file to the Recycle Bin. Fails and keeps the file when its drive has no Recycle Bin,
    // rather than deleting it for good. A file that does not exist counts as recycled.
    [[nodiscard]] virtual base::Status Recycle(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual base::Status CreateDirectories(const std::filesystem::path& path) = 0;
    // Deletes a folder with everything in it: a stream's parts folder. A folder that does not
    // exist counts as deleted.
    [[nodiscard]] virtual base::Status RemoveFolder(const std::filesystem::path& path) = 0;
    // Writes a small file whole: a web page the Site Grabber saves (M4 design decision 11). An
    // existing file is replaced only with `replace`; otherwise the call fails with AlreadyExists
    // and the file stays as it was.
    [[nodiscard]] virtual base::Status WriteFile(const std::filesystem::path& path, std::string_view bytes,
                                                 bool replace) = 0;
};

// Protects passwords at rest (DPAPI for the current user in production).
class SecretProtector {
public:
    virtual ~SecretProtector() = default;
    [[nodiscard]] virtual base::Result<std::vector<std::uint8_t>> Protect(std::string_view secret) = 0;
    [[nodiscard]] virtual base::Result<std::string> Unprotect(std::span<const std::uint8_t> protectedSecret) = 0;
};

// Random identifiers that may leave the process (design section 5: public_id).
class IdGenerator {
public:
    virtual ~IdGenerator() = default;
    [[nodiscard]] virtual std::string NewPublicId() = 0;
};

// The user's time zone, for schedules given in local time (M4 design section 4). Local times are
// milliseconds since 1970-01-01 00:00 in local time, counted as if every day had 24 hours.
class TimeZone {
public:
    virtual ~TimeZone() = default;
    // A local time that does not exist (skipped when clocks go forward) or exists twice (repeated
    // when they go back) gives one of the instants near it; schedules accept either.
    [[nodiscard]] virtual std::int64_t LocalToUnixMs(std::int64_t localMs) const = 0;
    [[nodiscard]] virtual std::int64_t UnixToLocalMs(std::int64_t unixMs) const = 0;
};

// Wakes the computer from sleep at a scheduled queue start, where Windows allows wake timers
// (checklist Q-07). One time at most is armed; arming again replaces it.
class WakeTimer {
public:
    virtual ~WakeTimer() = default;
    virtual void Arm(std::int64_t unixMs) = 0;
    virtual void Disarm() = 0;
};

// Keeps Windows awake while transfers run (checklist ENG-17).
class PowerRequests {
public:
    virtual ~PowerRequests() = default;
    virtual void SetTransfersActive(bool active) = 0;
};

// Chooses the proxy for one address according to Options > Proxy (checklist O-06). Returns a
// libcurl proxy URL, or nothing for a direct connection.
// What a proxy lookup knows at once.
struct ProxyLookup {
    bool pending = false;              // a lookup runs; its answer arrives as a ProxyResolved event
    std::optional<std::string> proxy;  // when not pending; nothing: connect directly
};

// Picks the proxy for each run. Lookups that may take long (PAC scripts, WPAD detection) must not
// block the caller, which is the core thread: they answer later through the engine listener.
class ProxyResolver {
public:
    virtual ~ProxyResolver() = default;
    // Called at startup and whenever settings change.
    virtual void Configure(const model::Settings& settings) = 0;
    [[nodiscard]] virtual ProxyLookup Resolve(model::DownloadId id, std::int64_t epoch, const std::string& url) = 0;
};

}  // namespace ldm::core
