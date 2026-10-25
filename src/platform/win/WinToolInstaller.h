#pragma once

#include "core/ToolInstaller.h"
#include "core/TransferEngine.h"
#include "platform/win/SiteToolsFolder.h"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

// Installs yt-dlp and Deno into the tools folder (video-sites design, section 6), on its own
// thread: the archive's SHA-256 must match the published one, Windows' tar.exe unpacks it into a
// new folder, the program must answer --version with the release's version, and only then does
// the new folder take the old one's place. Anything that fails leaves the installed tool as it was.
namespace ldm::platform {

// The SHA-256 of `bytes` in lower-case hex. Empty when Windows' hashing failed.
[[nodiscard]] std::string Sha256Hex(std::string_view bytes);

class WinToolInstaller final : public core::ToolInstaller {
public:
    // `folder` and `listener` must outlive this object.
    WinToolInstaller(SiteToolsFolder& folder, core::EngineListener& listener);
    // Joins the worker after the job it runs; queued jobs are dropped unanswered.
    ~WinToolInstaller() override;
    WinToolInstaller(const WinToolInstaller&) = delete;
    WinToolInstaller& operator=(const WinToolInstaller&) = delete;

    void Install(core::ToolInstallRequest request) override;
    void Remove() override;
    [[nodiscard]] std::optional<std::string> InstalledVersion(core::SiteTool tool) const override;

private:
    struct RemoveAll {};
    using Job = std::variant<core::ToolInstallRequest, RemoveAll>;

    void RunWorker();
    [[nodiscard]] std::optional<std::string> InstallNow(const core::ToolInstallRequest& request);
    [[nodiscard]] std::optional<std::string> RemoveNow();

    SiteToolsFolder& folder_;
    core::EngineListener& listener_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    bool stopping_ = false;
    std::thread worker_;  // last: starts after everything above exists
};

}  // namespace ldm::platform
