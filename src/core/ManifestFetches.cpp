#include "core/ManifestFetches.h"

#include "core/Url.h"
#include "model/Cookie.h"

#include <utility>

namespace ldm::core {

namespace {

// Lookups still waiting for a proxy beyond this are refused; a page cannot pile them up.
constexpr size_t kMaxWaiting = 32;

FetchFinished Refused(std::int64_t fetchId, std::string message)
{
    FetchFinished finished;
    finished.fetchId = fetchId;
    finished.failure = TransferFailure{FailureReason::Internal, FailureClass::Fatal, 0, std::move(message)};
    return finished;
}

}  // namespace

ManifestFetches::ManifestFetches(TransferEngine& engine, ProxyResolver& proxy, CoreObserver& observer)
    : engine_(engine), proxy_(proxy), observer_(observer)
{
}

void ManifestFetches::Start(FetchManifestCommand command, const std::string& userAgent)
{
    const auto url = ParseUrl(command.url);
    if (command.fetchId <= 0 || !url || (url->scheme != "http" && url->scheme != "https")) {
        observer_.OnNotification(Refused(command.fetchId, "not an http or https address"));
        return;
    }
    if (waitingForProxy_.size() >= kMaxWaiting || waitingForProxy_.contains(command.fetchId)) {
        observer_.OnNotification(Refused(command.fetchId, "too many manifest fetches at once"));
        return;
    }
    FetchRequest request;
    request.fetchId = command.fetchId;
    request.url = std::move(command.url);
    request.referer = std::move(command.referer);
    request.userAgent = command.userAgent.value_or(userAgent);
    request.cookieLines = model::CookieLines(model::CookiesToText(command.cookies));
    request.maxBytes = command.maxBytes;

    const ProxyLookup lookup = proxy_.Resolve(model::DownloadId{-request.fetchId}, 0, request.url);
    if (lookup.pending) {
        const std::int64_t id = request.fetchId;
        waitingForProxy_.emplace(id, std::move(request));
        return;
    }
    request.proxy = lookup.proxy;
    engine_.Fetch(std::move(request));
}

void ManifestFetches::OnProxyResolved(model::DownloadId id, const std::optional<std::string>& proxy)
{
    const auto waiting = waitingForProxy_.find(-id.value);
    if (waiting == waitingForProxy_.end()) {
        return;
    }
    FetchRequest request = std::move(waiting->second);
    waitingForProxy_.erase(waiting);
    request.proxy = proxy;
    engine_.Fetch(std::move(request));
}

void ManifestFetches::OnFinished(FetchFinished finished)
{
    observer_.OnNotification(std::move(finished));
}

}  // namespace ldm::core
