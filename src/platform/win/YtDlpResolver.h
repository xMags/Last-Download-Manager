#pragma once

#include "core/SiteResolver.h"
#include "core/TransferEngine.h"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// The site resolver with yt-dlp (video-sites design, sections 3 and 6): runs yt-dlp.exe from LDM's
// tools folder with Deno for the sites that need a JavaScript runtime, in its own process with the
// limits of RunChildProcess, and reads its JSON answer with media::ParseSiteVideo. yt-dlp only
// says what a page offers; it never downloads anything for LDM.
//
// Threads: Resolve() and Cancel() are called on the core thread; answers go to `listener` from the
// workers. At most kWorkers lookups run at once, in the order asked.
namespace ldm::platform {

struct YtDlpTools {
    std::filesystem::path ytDlp;         // yt-dlp.exe of the folder build
    std::filesystem::path deno;          // deno.exe
    std::filesystem::path cacheFolder;   // yt-dlp's cache, LDM's own
    std::filesystem::path ffmpegFolder;  // the folder of the ffmpeg.exe LDM ships
};

// The words to show for a failed lookup, from what yt-dlp printed on standard error, and whether
// a newer yt-dlp may fix it. Pure, for tests.
struct ResolverFailure {
    core::SiteLookupFinished::Problem problem = core::SiteLookupFinished::Problem::Resolver;
    std::string message;
};
[[nodiscard]] ResolverFailure ReadResolverFailure(std::string_view errors);

// yt-dlp's arguments for one lookup. Pure, for tests.
// `cookieFile`: the Netscape cookies file of the request's sign-in, when it has one.
[[nodiscard]] std::vector<std::wstring> YtDlpArguments(const YtDlpTools& tools, const core::SiteLookupRequest& request,
                                                       const std::optional<std::filesystem::path>& cookieFile = std::nullopt);

class YtDlpResolver final : public core::SiteResolver {
public:
    // `listener` and `toolsLock` must outlive this object. A run holds `toolsLock` shared, so the
    // programs are not replaced while they run (SiteToolsFolder::Lock); tests may pass none.
    YtDlpResolver(YtDlpTools tools, core::EngineListener& listener, std::shared_mutex* toolsLock = nullptr);
    // Ends running lookups and joins the workers; lookups still queued are dropped unanswered, as
    // the core is shutting down.
    ~YtDlpResolver() override;
    YtDlpResolver(const YtDlpResolver&) = delete;
    YtDlpResolver& operator=(const YtDlpResolver&) = delete;

    void Resolve(core::SiteLookupRequest request) override;
    void Cancel(std::int64_t lookupId) override;

    static constexpr int kWorkers = 2;
    // A lookup normally takes about 3 s; a slow site or a cold Deno start can take longer.
    static constexpr std::chrono::seconds kLookupTimeout{45};

private:
    void RunWorker();
    [[nodiscard]] core::SiteLookupFinished LookUp(const core::SiteLookupRequest& request, HANDLE stop) const;
    void Answer(core::SiteLookupFinished finished);

    const YtDlpTools tools_;
    core::EngineListener& listener_;
    std::shared_mutex* const toolsLock_;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<core::SiteLookupRequest> queue_;
    std::map<std::int64_t, HANDLE> running_;  // lookup id to the stop event its worker owns
    bool stopping_ = false;
    std::vector<std::thread> workers_;  // last: they start after everything above exists
};

}  // namespace ldm::platform
