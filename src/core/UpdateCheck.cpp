#include "core/UpdateCheck.h"

#include "core/Url.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <utility>
#include <vector>

namespace ldm::core {

namespace {

constexpr std::size_t kMaxFeedBytes = 1024 * 1024;
constexpr std::size_t kMaxVersionLength = 64;

struct Version {
    std::uint64_t major = 0;
    std::uint64_t minor = 0;
    std::uint64_t patch = 0;
    std::vector<std::string> pre;  // dot-separated pre-release identifiers; empty for a release
};

std::optional<std::uint64_t> Number(std::string_view text)
{
    if (text.empty() || (text.size() > 1 && text.front() == '0')) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<Version> ParseVersion(std::string_view text)
{
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
        text.remove_prefix(1);
    }
    if (text.empty() || text.size() > kMaxVersionLength) {
        return std::nullopt;
    }
    // Build metadata ("+...") does not take part in the order (SemVer 2.0.0, item 10).
    text = text.substr(0, text.find('+'));
    std::string_view pre;
    if (const std::size_t dash = text.find('-'); dash != std::string_view::npos) {
        pre = text.substr(dash + 1);
        text = text.substr(0, dash);
        if (pre.empty()) {
            return std::nullopt;
        }
    }
    Version version;
    std::uint64_t* const parts[] = {&version.major, &version.minor, &version.patch};
    for (std::size_t i = 0; i < 3; ++i) {
        const std::size_t dot = i < 2 ? text.find('.') : std::string_view::npos;
        if (i < 2 && dot == std::string_view::npos) {
            return std::nullopt;
        }
        const auto number = Number(text.substr(0, dot));
        if (!number) {
            return std::nullopt;
        }
        *parts[i] = *number;
        text = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    }
    while (!pre.empty()) {
        const std::size_t dot = pre.find('.');
        const std::string_view part = pre.substr(0, dot);
        if (part.empty()) {
            return std::nullopt;
        }
        for (const char c : part) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-')) {
                return std::nullopt;
            }
        }
        version.pre.emplace_back(part);
        pre = dot == std::string_view::npos ? std::string_view{} : pre.substr(dot + 1);
    }
    return version;
}

// SemVer 2.0.0 precedence: negative, zero or positive.
int Compare(const Version& a, const Version& b)
{
    for (const auto& [x, y] : {std::pair{a.major, b.major}, std::pair{a.minor, b.minor}, std::pair{a.patch, b.patch}}) {
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    if (a.pre.empty() || b.pre.empty()) {
        return a.pre.empty() == b.pre.empty() ? 0 : (a.pre.empty() ? 1 : -1);
    }
    for (std::size_t i = 0; i < a.pre.size() && i < b.pre.size(); ++i) {
        const auto x = Number(a.pre[i]);
        const auto y = Number(b.pre[i]);
        if (x && y) {
            if (*x != *y) {
                return *x < *y ? -1 : 1;
            }
        } else if (x || y) {
            return x ? -1 : 1;  // numeric identifiers sort before others
        } else if (a.pre[i] != b.pre[i]) {
            return a.pre[i] < b.pre[i] ? -1 : 1;
        }
    }
    return a.pre.size() == b.pre.size() ? 0 : (a.pre.size() < b.pre.size() ? -1 : 1);
}

}  // namespace

std::optional<LatestRelease> ParseLatestRelease(std::string_view json)
{
    if (json.size() > kMaxFeedBytes) {
        return std::nullopt;
    }
    const nlohmann::json document = nlohmann::json::parse(json, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return std::nullopt;
    }
    const auto tag = document.find("tag_name");
    const auto page = document.find("html_url");
    if (tag == document.end() || !tag->is_string() || page == document.end() || !page->is_string()) {
        return std::nullopt;
    }
    const auto draft = document.find("draft");
    if (draft != document.end() && draft->is_boolean() && draft->get<bool>()) {
        return std::nullopt;
    }
    std::string version = tag->get<std::string>();
    if (!ParseVersion(version)) {
        return std::nullopt;
    }
    if (version.front() == 'v' || version.front() == 'V') {
        version.erase(0, 1);
    }
    // The notice opens this address, so only GitHub's own pages qualify.
    const std::string& pageUrl = page->get_ref<const std::string&>();
    const auto url = ParseUrl(pageUrl);
    if (!url || url->scheme != "https" || url->host != "github.com" || url->hasUserInfo || url->port) {
        return std::nullopt;
    }
    LatestRelease release;
    release.version = std::move(version);
    release.pageUrl = pageUrl;
    const auto prerelease = document.find("prerelease");
    release.prerelease = prerelease != document.end() && prerelease->is_boolean() && prerelease->get<bool>();
    return release;
}

bool IsNewerVersion(std::string_view candidate, std::string_view current)
{
    const auto newer = ParseVersion(candidate);
    const auto running = ParseVersion(current);
    if (!newer || !running) {
        return false;
    }
    if (!newer->pre.empty() && running->pre.empty()) {
        return false;  // pre-releases are offered only to pre-release users
    }
    return Compare(*newer, *running) > 0;
}

UpdateCheck::UpdateCheck(TransferEngine& engine, ProxyResolver& proxy, std::string feedUrl, std::string currentVersion,
                         std::chrono::steady_clock::time_point startedAt)
    : engine_(engine),
      proxy_(proxy),
      feedUrl_(std::move(feedUrl)),
      currentVersion_(std::move(currentVersion)),
      startedAt_(startedAt)
{
}

std::chrono::steady_clock::time_point UpdateCheck::NextDeadline(const model::Settings& settings) const
{
    if (feedUrl_.empty() || !settings.updatesCheckEnabled || running_) {
        return std::chrono::steady_clock::time_point::max();
    }
    // Looked at again every hour; Tick decides whether a check is due.
    const auto first = startedAt_ + kUpdateFirstDelay;
    return lastTick_ ? std::max(first, *lastTick_ + std::chrono::hours(1)) : first;
}

void UpdateCheck::Tick(std::chrono::steady_clock::time_point now, std::int64_t nowUnixMs, const model::Settings& settings)
{
    if (feedUrl_.empty() || !settings.updatesCheckEnabled || running_ || now < startedAt_ + kUpdateFirstDelay) {
        return;
    }
    if (lastTick_ && now < *lastTick_ + std::chrono::hours(1)) {
        return;
    }
    lastTick_ = now;
    const std::int64_t interval = std::chrono::duration_cast<std::chrono::milliseconds>(kUpdateInterval).count();
    if (settings.updatesLastCheckUnixMs > 0 && nowUnixMs - settings.updatesLastCheckUnixMs < interval &&
        nowUnixMs >= settings.updatesLastCheckUnixMs) {
        return;
    }
    if (lastAttempt_ && now < *lastAttempt_ + kUpdateRetry) {
        return;
    }
    lastAttempt_ = now;
    Start(false);
}

void UpdateCheck::CheckNow()
{
    if (running_) {
        manual_ = true;  // the check under way answers this request too
        return;
    }
    Start(true);
}

void UpdateCheck::Start(bool manual)
{
    running_ = true;
    manual_ = manual;
    const ProxyLookup lookup = proxy_.Resolve(model::DownloadId{kUpdateFetchId}, 0, feedUrl_);
    if (lookup.pending) {
        waitingForProxy_ = true;
        return;
    }
    OnProxyResolved(lookup.proxy);
}

void UpdateCheck::OnProxyResolved(const std::optional<std::string>& proxy)
{
    if (!running_) {
        return;
    }
    waitingForProxy_ = false;
    FetchRequest request;
    request.fetchId = kUpdateFetchId;
    request.url = feedUrl_;
    request.userAgent = "LastDownloadManager/" + currentVersion_;
    request.proxy = proxy;
    request.maxBytes = kMaxFeedBytes;
    engine_.Fetch(std::move(request));
}

std::optional<UpdateCheckOutcome> UpdateCheck::OnFetched(const FetchFinished& finished, const model::Settings& settings)
{
    if (finished.fetchId != kUpdateFetchId || !running_) {
        return std::nullopt;
    }
    running_ = false;
    UpdateCheckOutcome outcome;
    outcome.manual = manual_;
    manual_ = false;
    if (finished.failure) {
        outcome.error = finished.failure->message.empty() ? "no answer" : finished.failure->message;
        return outcome;
    }
    const auto release = ParseLatestRelease(finished.body);
    if (!release) {
        outcome.error = "the answer is not a release";
        return outcome;
    }
    if (IsNewerVersion(release->version, currentVersion_) &&
        (outcome.manual || release->version != settings.updatesSkippedVersion)) {
        outcome.newer = release;
    }
    return outcome;
}

}  // namespace ldm::core
