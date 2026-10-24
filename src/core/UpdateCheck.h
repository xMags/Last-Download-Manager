#pragma once

#include "core/Ports.h"
#include "core/TransferEngine.h"
#include "model/Settings.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The update check (checklist X-05, M5 design section 3): opt-in, against LDM's GitHub releases,
// a notice and never a download.
namespace ldm::core {

// What GitHub's "latest release" answer says, validated as untrusted input.
struct LatestRelease {
    std::string version;  // the tag without its leading "v": "5.1.0" or "5.1.0-beta.2"
    std::string pageUrl;  // the release's page, always under kReleasesPage
    bool prerelease = false;
};

// The notice opens only pages below this: LDM's own releases on GitHub.
inline constexpr std::string_view kReleasesPage = "https://github.com/xMags/Last-Download-Manager/releases/";

[[nodiscard]] std::optional<LatestRelease> ParseLatestRelease(std::string_view json);

// Semantic versions (MAJOR.MINOR.PATCH with an optional "-pre.release" part). A pre-release of
// `candidate` counts only when `current` is itself a pre-release. False when either is malformed.
[[nodiscard]] bool IsNewerVersion(std::string_view candidate, std::string_view current);

// An automatic check runs this long after LDM starts, and then again once this much time passed
// since the last check that got an answer.
inline constexpr std::chrono::seconds kUpdateFirstDelay{60};
inline constexpr std::chrono::hours kUpdateInterval{14 * 24};
// A check that got no answer is tried again after this.
inline constexpr std::chrono::hours kUpdateRetry{24};

// The one fetch of the update check uses this id, and its proxy lookup the same DownloadId, so the
// coordinator can tell its answers from the playlists' (small negative ids) and the Grabber's.
inline constexpr std::int64_t kUpdateFetchId = -(std::int64_t{1} << 38);

// The answer of a check: sent by the coordinator as an UpdateCheckFinished notification.
struct UpdateCheckOutcome {
    bool manual = false;
    std::optional<LatestRelease> newer;  // a newer release than the running version
    std::optional<std::string> error;    // the check got no usable answer
};

// Runs on the core thread inside the coordinator: decides when to check, sends the fetch through
// the engine with the proxy downloads use, and reads the answer.
class UpdateCheck {
public:
    // `feedUrl` empty turns the check off (tests, development builds). `startedAt`: when LDM started.
    UpdateCheck(TransferEngine& engine, ProxyResolver& proxy, std::string feedUrl, std::string currentVersion,
                std::chrono::steady_clock::time_point startedAt);

    // Starts an automatic check when the settings allow it and one is due.
    void Tick(std::chrono::steady_clock::time_point now, std::int64_t nowUnixMs, const model::Settings& settings);
    // Help > Check for updates: now, whatever the settings say.
    void CheckNow();
    [[nodiscard]] std::chrono::steady_clock::time_point NextDeadline(const model::Settings& settings) const;

    // The fetch's proxy answer.
    void OnProxyResolved(const std::optional<std::string>& proxy);
    [[nodiscard]] bool WaitingForProxy() const noexcept { return waitingForProxy_; }
    // The fetch's answer. Nothing when it was not this check's; the skipped version is left out of
    // automatic checks.
    [[nodiscard]] std::optional<UpdateCheckOutcome> OnFetched(const FetchFinished& finished, const model::Settings& settings);

    [[nodiscard]] bool Running() const noexcept { return running_; }

private:
    void Start(bool manual);

    TransferEngine& engine_;
    ProxyResolver& proxy_;
    std::string feedUrl_;
    std::string currentVersion_;
    std::chrono::steady_clock::time_point startedAt_;
    std::optional<std::chrono::steady_clock::time_point> lastTick_;     // when Tick last looked
    std::optional<std::chrono::steady_clock::time_point> lastAttempt_;  // of an automatic check
    bool running_ = false;
    bool manual_ = false;
    bool waitingForProxy_ = false;
};

}  // namespace ldm::core
