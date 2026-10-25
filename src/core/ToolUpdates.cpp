#include "core/ToolUpdates.h"

#include <algorithm>
#include <utility>

namespace ldm::core {

namespace {

// GitHub's API answers only requests that name their program.
constexpr const char* kUserAgent = "Last-Download-Manager";
constexpr std::size_t kMaxReleaseBytes = 1024 * 1024;
constexpr std::size_t kMaxChecksumBytes = 64 * 1024;
constexpr SiteTool kTools[] = {SiteTool::YtDlp, SiteTool::Deno};

std::chrono::milliseconds IntervalOf(SiteTool tool)
{
    return tool == SiteTool::YtDlp ? std::chrono::milliseconds(ToolUpdates::kYtDlpInterval)
                                   : std::chrono::milliseconds(ToolUpdates::kDenoInterval);
}

std::int64_t LastCheck(SiteTool tool, const model::Settings& settings)
{
    return tool == SiteTool::YtDlp ? settings.siteVideosYtDlpCheckUnixMs : settings.siteVideosDenoCheckUnixMs;
}

std::string Named(SiteTool tool, std::string_view what)
{
    return std::string(what) + " " + std::string(SiteToolName(tool));
}

}  // namespace

ToolUpdates::ToolUpdates(TransferEngine& engine, ProxyResolver& proxy, ToolInstaller& installer)
    : engine_(engine), proxy_(proxy), installer_(installer)
{
}

const std::optional<std::string>& ToolUpdates::Installed(SiteTool tool) const
{
    auto it = installed_.find(tool);
    if (it == installed_.end()) {
        it = installed_.emplace(tool, installer_.InstalledVersion(tool)).first;
    }
    return it->second;
}

std::optional<SiteTool> ToolUpdates::DueTool(std::chrono::steady_clock::time_point now, std::int64_t nowUnixMs,
                                             const model::Settings& settings) const
{
    for (const SiteTool tool : kTools) {
        if (forced_.contains(tool)) {
            return tool;
        }
    }
    for (const SiteTool tool : kTools) {
        if (const auto failed = failedAt_.find(tool); failed != failedAt_.end() && now - failed->second < kRetryAfter) {
            continue;
        }
        const std::int64_t last = LastCheck(tool, settings);
        // A clock set back makes the last check look like the future: checked again then.
        const bool stale = nowUnixMs < last || nowUnixMs - last >= IntervalOf(tool).count();
        if (!Installed(tool) || stale) {
            return tool;
        }
    }
    return std::nullopt;
}

void ToolUpdates::Tick(std::chrono::steady_clock::time_point now, std::int64_t nowUnixMs, const model::Settings& settings)
{
    if (!settings.siteVideosEnabled) {
        job_.reset();  // its late answers find nothing
        forced_.clear();
        return;
    }
    if (job_ || removing_) {
        return;
    }
    if (const auto tool = DueTool(now, nowUnixMs, settings)) {
        forced_.erase(*tool);
        Begin(*tool);
    }
}

std::chrono::steady_clock::time_point ToolUpdates::NextDeadline(std::chrono::steady_clock::time_point now,
                                                                std::int64_t nowUnixMs,
                                                                const model::Settings& settings) const
{
    if (!settings.siteVideosEnabled || job_ || removing_) {
        return std::chrono::steady_clock::time_point::max();  // answers, not time, move it on
    }
    if (DueTool(now, nowUnixMs, settings)) {
        return now;
    }
    auto deadline = std::chrono::steady_clock::time_point::max();
    for (const SiteTool tool : kTools) {
        const std::int64_t due = LastCheck(tool, settings) + IntervalOf(tool).count();
        deadline = std::min(deadline, now + std::chrono::milliseconds(std::max<std::int64_t>(due - nowUnixMs, 0)));
        if (const auto failed = failedAt_.find(tool); failed != failedAt_.end()) {
            deadline = std::min(deadline, failed->second + kRetryAfter);
        }
    }
    return deadline;
}

void ToolUpdates::CheckNow()
{
    forced_.insert(SiteTool::YtDlp);
    forced_.insert(SiteTool::Deno);
}

void ToolUpdates::Remove()
{
    job_.reset();
    forced_.clear();
    removing_ = true;
    installer_.Remove();
}

void ToolUpdates::ResolverTrouble(std::chrono::steady_clock::time_point now)
{
    if (lastTrouble_ && now - *lastTrouble_ < kRetryAfter) {
        return;
    }
    lastTrouble_ = now;
    forced_.insert(SiteTool::YtDlp);
}

void ToolUpdates::Begin(SiteTool tool)
{
    job_ = Job{};
    job_->tool = tool;
    Fetch(std::string(SiteToolFeedUrl(tool)), kMaxReleaseBytes, std::nullopt);
}

void ToolUpdates::Fetch(std::string url, std::size_t maxBytes, std::optional<std::chrono::seconds> timeout)
{
    FetchRequest request;
    request.fetchId = kToolFetchIdTop - (fetchCount_++ % kToolFetchIdCount);
    request.url = std::move(url);
    request.userAgent = kUserAgent;
    request.maxBytes = maxBytes;
    request.totalTimeout = timeout;
    job_->fetchId = request.fetchId;
    const ProxyLookup lookup = proxy_.Resolve(model::DownloadId{request.fetchId}, 0, request.url);
    if (lookup.pending) {
        job_->awaitingProxy = true;
        job_->waiting = std::move(request);
        return;
    }
    request.proxy = lookup.proxy;
    engine_.Fetch(std::move(request));
}

void ToolUpdates::OnProxyResolved(std::int64_t fetchId, const std::optional<std::string>& proxy)
{
    if (!job_ || !job_->awaitingProxy || job_->fetchId != fetchId || !job_->waiting) {
        return;
    }
    FetchRequest request = std::move(*job_->waiting);
    job_->waiting.reset();
    job_->awaitingProxy = false;
    request.proxy = proxy;
    engine_.Fetch(std::move(request));
}

std::optional<SiteTool> ToolUpdates::OnFetched(const FetchFinished& finished, std::chrono::steady_clock::time_point now)
{
    if (!job_ || job_->fetchId != finished.fetchId || job_->awaitingProxy) {
        return std::nullopt;  // abandoned, or an older fetch
    }
    const SiteTool tool = job_->tool;
    if (finished.failure) {
        Fail(Named(tool, "Could not download") + ": " + finished.failure->message, now);
        return std::nullopt;
    }
    switch (job_->stage) {
    case Stage::Release: {
        const auto release = ParseToolRelease(tool, finished.body);
        if (!release) {
            Fail("GitHub's answer about " + std::string(SiteToolName(tool)) + " could not be read.", now);
            return std::nullopt;
        }
        if (Installed(tool) == release->version) {
            job_.reset();
            failedAt_.erase(tool);
            return tool;  // current
        }
        job_->release = *release;
        job_->stage = Stage::Checksum;
        Fetch(release->checksumUrl, kMaxChecksumBytes, std::nullopt);
        return tool;
    }
    case Stage::Checksum: {
        const auto sha256 = ParseToolChecksum(tool, finished.body, job_->release.archiveName);
        if (!sha256) {
            Fail(Named(tool, "The checksum of") + " is missing from its release.", now);
            return std::nullopt;
        }
        job_->sha256 = *sha256;
        job_->stage = Stage::Archive;
        Fetch(job_->release.archiveUrl, static_cast<std::size_t>(job_->release.archiveBytes),
              std::chrono::duration_cast<std::chrono::seconds>(kArchiveTimeout));
        return std::nullopt;
    }
    case Stage::Archive: {
        if (static_cast<std::int64_t>(finished.body.size()) != job_->release.archiveBytes) {
            Fail(Named(tool, "The download of") + " was incomplete.", now);
            return std::nullopt;
        }
        job_->stage = Stage::Installing;
        installer_.Install(ToolInstallRequest{tool, job_->release.version, finished.body, job_->sha256});
        return std::nullopt;
    }
    case Stage::Installing:
        return std::nullopt;
    }
    return std::nullopt;
}

void ToolUpdates::OnInstalled(const ToolInstalled& installed, std::chrono::steady_clock::time_point now)
{
    if (installed.action == ToolInstalled::Action::Remove) {
        removing_ = false;
        installed_.clear();  // read again: what is left, if removing failed halfway
        if (installed.failure) {
            lastError_ = *installed.failure;
        } else {
            lastError_.clear();
        }
        return;
    }
    if (!job_ || job_->stage != Stage::Installing || job_->tool != installed.tool) {
        installed_.erase(installed.tool);  // an abandoned install may still have changed the folder
        return;
    }
    if (installed.failure) {
        installed_.erase(installed.tool);
        Fail(*installed.failure, now);
        return;
    }
    installed_[installed.tool] = installed.version;
    failedAt_.erase(installed.tool);
    lastError_.clear();
    job_.reset();
}

void ToolUpdates::Fail(std::string message, std::chrono::steady_clock::time_point now)
{
    failedAt_[job_->tool] = now;
    lastError_ = std::move(message);
    job_.reset();
}

SiteToolsView ToolUpdates::View() const
{
    SiteToolsView view;
    view.ytDlpVersion = Installed(SiteTool::YtDlp);
    view.denoVersion = Installed(SiteTool::Deno);
    view.lastError = lastError_;
    if (removing_) {
        view.state = SiteToolsView::State::Removing;
    } else if (job_) {
        view.tool = job_->tool;
        switch (job_->stage) {
        case Stage::Release:
        case Stage::Checksum:
            view.state = SiteToolsView::State::Checking;
            break;
        case Stage::Archive:
            view.state = SiteToolsView::State::Downloading;
            view.archiveBytes = job_->release.archiveBytes;
            break;
        case Stage::Installing:
            view.state = SiteToolsView::State::Installing;
            break;
        }
    }
    return view;
}

}  // namespace ldm::core
