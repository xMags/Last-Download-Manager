#pragma once

#include "core/TransferEngine.h"
#include "model/Ids.h"

#include <cstdint>
#include <optional>
#include <string>

// The port to the site resolver (yt-dlp; video-sites design, section 3): asks what a video page of
// a supported site offers. Implemented in platform/win with yt-dlp on worker threads; the core
// only builds the request and reads the answer.
namespace ldm::core {

struct SiteLookupRequest {
    std::int64_t lookupId = 0;  // at least 1
    std::string pageUrl;        // core::FindSiteVideo's canonical address, nothing a page wrote
    std::optional<std::string> proxy;  // as downloads use it; nothing: connect directly
};

// Lookups and their proxy questions use DownloadIds in their own range, below the update check's
// and above the Grabber's, so the coordinator can tell their events from everything else.
inline constexpr std::int64_t kSiteLookupIdBase = -(std::int64_t{1} << 39);
inline constexpr std::int64_t kMaxSiteLookupId = (std::int64_t{1} << 39) - 1;

[[nodiscard]] constexpr model::DownloadId SiteLookupKey(std::int64_t lookupId) noexcept
{
    return model::DownloadId{kSiteLookupIdBase - lookupId};
}
[[nodiscard]] constexpr bool IsSiteLookupKey(model::DownloadId id) noexcept
{
    return id.value < kSiteLookupIdBase && id.value >= kSiteLookupIdBase - kMaxSiteLookupId;
}
[[nodiscard]] constexpr std::int64_t SiteLookupIdOf(model::DownloadId id) noexcept
{
    return kSiteLookupIdBase - id.value;
}

// Every method is thread-safe and returns at once. Each Resolve is answered by exactly one
// SiteLookupFinished event, through the engine listener, with SiteLookupKey(lookupId) as its id.
class SiteResolver {
public:
    virtual ~SiteResolver() = default;
    virtual void Resolve(SiteLookupRequest request) = 0;
    // Stops a running or queued lookup; it is then answered with `stopped` set. Nothing happens
    // when it already answered.
    virtual void Cancel(std::int64_t lookupId) = 0;
};

}  // namespace ldm::core
