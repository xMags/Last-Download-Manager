#pragma once

#include "core/Ports.h"
#include "core/SiteTools.h"
#include "core/ToolInstaller.h"
#include "core/TransferEngine.h"
#include "model/Ids.h"
#include "model/Settings.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>

// Keeps yt-dlp and Deno installed and current while site videos are turned on (video-sites
// design, section 6). Runs on the core thread inside the coordinator: decides when to check,
// fetches each release's answer, checksum file and archive through the engine with the proxy
// downloads use, and hands the archive to the tool installer. One tool at a time.
namespace ldm::core {

// What Options shows.
struct SiteToolsView {
    enum class State {
        Idle,
        Checking,     // asking GitHub for the latest release
        Downloading,  // the archive of `tool`, `archiveBytes` long
        Installing,
        Removing,
    };
    State state = State::Idle;
    SiteTool tool = SiteTool::YtDlp;
    std::int64_t archiveBytes = 0;
    std::optional<std::string> ytDlpVersion;  // installed versions
    std::optional<std::string> denoVersion;
    std::string lastError;  // of the last attempt that failed, until one succeeds
};

// The fetches' ids, counting down from the top; their proxy questions use the same DownloadIds.
// Just below the update check's id, far above the Grabber's and the site lookups'.
inline constexpr std::int64_t kToolFetchIdTop = -(std::int64_t{1} << 38) - 1;
inline constexpr std::int64_t kToolFetchIdCount = 1024;
[[nodiscard]] constexpr bool IsToolFetchId(std::int64_t id) noexcept
{
    return id <= kToolFetchIdTop && id > kToolFetchIdTop - kToolFetchIdCount;
}

class ToolUpdates {
public:
    ToolUpdates(TransferEngine& engine, ProxyResolver& proxy, ToolInstaller& installer);

    // Starts due work when site videos are on. `nowUnixMs` is compared with the last checks in
    // `settings`; turning site videos off abandons work under way (the tools stay).
    void Tick(std::chrono::steady_clock::time_point now, std::int64_t nowUnixMs, const model::Settings& settings);
    [[nodiscard]] std::chrono::steady_clock::time_point NextDeadline(std::chrono::steady_clock::time_point now,
                                                                     std::int64_t nowUnixMs,
                                                                     const model::Settings& settings) const;
    // Options > Check now: both tools, at the next Tick, whenever they were last checked.
    void CheckNow();
    // Options > Remove components. Work under way is abandoned.
    void Remove();
    // A lookup failed in a way a newer yt-dlp may fix: checks for one, at most once an hour.
    void ResolverTrouble(std::chrono::steady_clock::time_point now);

    void OnProxyResolved(std::int64_t fetchId, const std::optional<std::string>& proxy);
    // A fetch's answer. Returns the tool whose release answer arrived, so the caller stores the
    // time of the check; nothing otherwise.
    [[nodiscard]] std::optional<SiteTool> OnFetched(const FetchFinished& finished, std::chrono::steady_clock::time_point now);
    void OnInstalled(const ToolInstalled& installed, std::chrono::steady_clock::time_point now);

    [[nodiscard]] SiteToolsView View() const;

    static constexpr std::chrono::hours kYtDlpInterval{24};
    static constexpr std::chrono::hours kDenoInterval{7 * 24};
    // After a failed automatic attempt, and between checks caused by lookup failures.
    static constexpr std::chrono::hours kRetryAfter{1};
    static constexpr std::chrono::minutes kArchiveTimeout{30};

private:
    enum class Stage {
        Release,
        Checksum,
        Archive,
        Installing,
    };
    struct Job {
        SiteTool tool = SiteTool::YtDlp;
        Stage stage = Stage::Release;
        std::int64_t fetchId = 0;
        bool awaitingProxy = false;
        std::optional<FetchRequest> waiting;  // the fetch, until its proxy is known
        ToolRelease release;
        std::string sha256;
    };

    [[nodiscard]] std::optional<SiteTool> DueTool(std::chrono::steady_clock::time_point now, std::int64_t nowUnixMs,
                                                  const model::Settings& settings) const;
    void Begin(SiteTool tool);
    void Fetch(std::string url, std::size_t maxBytes, std::optional<std::chrono::seconds> timeout);
    void Fail(std::string message, std::chrono::steady_clock::time_point now);
    [[nodiscard]] const std::optional<std::string>& Installed(SiteTool tool) const;

    TransferEngine& engine_;
    ProxyResolver& proxy_;
    ToolInstaller& installer_;
    std::optional<Job> job_;
    bool removing_ = false;
    std::set<SiteTool> forced_;
    std::map<SiteTool, std::chrono::steady_clock::time_point> failedAt_;
    std::optional<std::chrono::steady_clock::time_point> lastTrouble_;
    mutable std::map<SiteTool, std::optional<std::string>> installed_;  // read from disk once, then kept
    std::string lastError_;
    std::int64_t fetchCount_ = 0;
};

}  // namespace ldm::core
