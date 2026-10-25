#include "platform/win/YtDlpResolver.h"

#include "base/Utf.h"
#include "media/SiteVideo.h"
#include "platform/win/ChildProcess.h"
#include "platform/win/PipeIo.h"

#include <algorithm>
#include <array>
#include <system_error>
#include <utility>

namespace ldm::platform {

namespace {

using Problem = core::SiteLookupFinished::Problem;

bool IsFile(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

bool Contains(std::string_view text, std::string_view part)
{
    return text.find(part) != std::string_view::npos;
}

// The last "ERROR: ..." line, without the prefix and without yt-dlp's "[site] id: " in front.
std::string LastError(std::string_view errors)
{
    std::string_view found;
    size_t start = 0;
    while (start < errors.size()) {
        const size_t end = std::min(errors.find('\n', start), errors.size());
        std::string_view line = errors.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.starts_with("ERROR: ")) {
            found = line.substr(7);
        }
        start = end + 1;
    }
    if (found.starts_with('[')) {
        if (const size_t close = found.find("] "); close != std::string_view::npos) {
            std::string_view rest = found.substr(close + 2);
            // "<id>: <message>", where the id has no spaces.
            if (const size_t colon = rest.find(": "); colon != std::string_view::npos && colon <= 64 &&
                rest.substr(0, colon).find(' ') == std::string_view::npos) {
                rest = rest.substr(colon + 2);
            }
            found = rest;
        }
    }
    std::string message(found.substr(0, 300));
    // yt-dlp points at its own bug tracker for some errors; that advice is not the user's to act on.
    if (const size_t advice = message.find(" please report"); advice != std::string::npos) {
        message.resize(advice);
    }
    while (!message.empty() && (message.back() == ' ' || message.back() == ';' || message.back() == '.')) {
        message.pop_back();
    }
    return message;
}

core::SiteLookupFinished Failure(Problem problem, std::string message)
{
    core::SiteLookupFinished finished;
    finished.problem = problem;
    finished.failure = std::move(message);
    return finished;
}

// One manual-reset event per running lookup, which Cancel and the destructor set.
OwnedHandle NewStopEvent()
{
    return OwnedHandle(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
}

}  // namespace

ResolverFailure ReadResolverFailure(std::string_view errors)
{
    const std::string error = LastError(errors);
    // Facts about the video itself: no newer yt-dlp changes them.
    constexpr std::array<std::string_view, 12> kVideoFacts{
        "Video unavailable",      "This video is unavailable", "Private video",      "This video is private",
        "has been removed",       "confirm your age",          "members-only",       "Join this channel",
        "Premieres in",           "live event will begin",     "not available in your country",
        "This video is not available"};
    for (const std::string_view fact : kVideoFacts) {
        if (Contains(error, fact)) {
            return {Problem::Video, error};
        }
    }
    if (Contains(error, "not a bot")) {
        return {Problem::Video, "The site asked to confirm that this is not a bot. Try again later."};
    }
    if (error.empty()) {
        return {Problem::Resolver, "The video site's page could not be read."};
    }
    return {Problem::Resolver, error};
}

std::vector<std::wstring> YtDlpArguments(const YtDlpTools& tools, const core::SiteLookupRequest& request)
{
    // Every behaviour is set here: no user configuration, no playlist, no progress or colours,
    // LDM's own cache and FFmpeg, and LDM's choice of proxy, where an empty one means directly.
    std::vector<std::wstring> arguments{
        L"--ignore-config", L"--no-playlist", L"--flat-playlist", L"--no-warnings", L"--no-progress",
        L"--color", L"never", L"--socket-timeout", L"15",
        L"--cache-dir", tools.cacheFolder.native(),
        L"--ffmpeg-location", tools.ffmpegFolder.native(),
        L"--js-runtimes", L"deno:" + tools.deno.native(),
        L"--proxy", base::ToUtf16(request.proxy.value_or(std::string())),
        L"--dump-single-json",
        // The address after "--" is never read as an option, whatever it starts with.
        L"--", base::ToUtf16(request.pageUrl)};
    return arguments;
}

YtDlpResolver::YtDlpResolver(YtDlpTools tools, core::EngineListener& listener)
    : tools_(std::move(tools)), listener_(listener)
{
    for (int i = 0; i < kWorkers; ++i) {
        workers_.emplace_back([this] { RunWorker(); });
    }
}

YtDlpResolver::~YtDlpResolver()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        queue_.clear();
        for (const auto& [id, stop] : running_) {
            ::SetEvent(stop);
        }
    }
    wake_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void YtDlpResolver::Resolve(core::SiteLookupRequest request)
{
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            return;
        }
        queue_.push_back(std::move(request));
    }
    wake_.notify_one();
}

void YtDlpResolver::Cancel(std::int64_t lookupId)
{
    {
        std::lock_guard lock(mutex_);
        if (const auto running = running_.find(lookupId); running != running_.end()) {
            ::SetEvent(running->second);
            return;  // its worker answers, with `stopped` set
        }
        const auto queued = std::find_if(queue_.begin(), queue_.end(),
                                         [lookupId](const core::SiteLookupRequest& r) { return r.lookupId == lookupId; });
        if (queued == queue_.end()) {
            return;
        }
        queue_.erase(queued);
    }
    core::SiteLookupFinished stopped;
    stopped.lookupId = lookupId;
    stopped.stopped = true;
    Answer(std::move(stopped));
}

void YtDlpResolver::RunWorker()
{
    while (true) {
        core::SiteLookupRequest request;
        OwnedHandle stop = NewStopEvent();
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                return;
            }
            request = std::move(queue_.front());
            queue_.pop_front();
            running_[request.lookupId] = stop.Get();
        }
        core::SiteLookupFinished answer = stop.Valid() ? LookUp(request, stop.Get())
                                                       : Failure(Problem::Resolver, "LDM could not start the lookup.");
        {
            std::lock_guard lock(mutex_);
            running_.erase(request.lookupId);
            if (stopping_) {
                return;  // the core no longer waits for answers
            }
        }
        if (stop.Valid() && ::WaitForSingleObject(stop.Get(), 0) == WAIT_OBJECT_0) {
            answer = core::SiteLookupFinished{};
            answer.stopped = true;
        }
        answer.lookupId = request.lookupId;
        Answer(std::move(answer));
    }
}

core::SiteLookupFinished YtDlpResolver::LookUp(const core::SiteLookupRequest& request, HANDLE stop) const
{
    if (!IsFile(tools_.ytDlp) || !IsFile(tools_.deno)) {
        return Failure(Problem::ToolsMissing, "The components for downloading videos from sites are not installed.");
    }
    std::error_code ignored;
    std::filesystem::create_directories(tools_.cacheFolder, ignored);

    ChildProcessRequest run;
    run.program = tools_.ytDlp;
    run.arguments = YtDlpArguments(tools_, request);
    run.workingDirectory = tools_.ytDlp.parent_path();
    run.separateErrors = true;
    run.limits.timeout = kLookupTimeout;
    run.limits.maxOutputBytes = media::kMaxSiteAnswerBytes + 1;  // one more: a cut answer is noticed
    const auto ran = RunChildProcess(run, stop);
    if (!ran) {
        return Failure(Problem::Resolver, "yt-dlp could not be started: " + ran.GetError().message);
    }
    const ChildProcessResult& result = ran.Value();
    if (result.end == ChildProcessEnd::Stopped) {
        core::SiteLookupFinished stopped;
        stopped.stopped = true;
        return stopped;
    }
    if (result.end == ChildProcessEnd::TimedOut) {
        return Failure(Problem::Resolver, "The video site took too long to answer.");
    }
    if (result.exitCode != 0) {
        ResolverFailure failure = ReadResolverFailure(result.errors);
        return Failure(failure.problem, std::move(failure.message));
    }
    if (result.outputTruncated) {
        return Failure(Problem::Video, "The video site's answer is too large.");
    }
    auto parsed = media::ParseSiteVideo(result.output);
    if (!parsed) {
        // An answer LDM cannot read at all is the resolver's; one it can read but refuses is the video's.
        const bool unreadable = parsed.GetError().code == base::ErrorCode::Protocol;
        return Failure(unreadable ? Problem::Resolver : Problem::Video, parsed.GetError().message);
    }
    core::SiteLookupFinished found;
    found.video = std::move(parsed).Value();
    return found;
}

void YtDlpResolver::Answer(core::SiteLookupFinished finished)
{
    const model::DownloadId key = core::SiteLookupKey(finished.lookupId);
    listener_.OnEngineEvent(core::EngineEvent{key, 0, std::move(finished)});
}

}  // namespace ldm::platform
