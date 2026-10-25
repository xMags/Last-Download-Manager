#pragma once

#include "core/TransferEngine.h"
#include "media/SiteVideo.h"
#include "model/Download.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Fresh addresses for a site video's tracks (video-sites design, section 8). The addresses a site
// gives expire (YouTube's after about six hours), so a site video asks the resolver about its page
// again before a run when they are about to, and after a run that failed the way an expired
// address fails. The same formats then continue from the bytes already on disk. Pure: the
// coordinator asks and stores.
namespace ldm::core {

// Fresh addresses are asked for when the current ones expire within this.
inline constexpr std::chrono::minutes kSiteAddressMargin{10};

// When an address stops working, from its "expire" parameter (seconds since 1970), in Unix
// milliseconds. Nothing when it does not say.
[[nodiscard]] std::optional<std::int64_t> AddressExpiresAtUnixMs(std::string_view url);

// A site video with a track whose address has expired or expires within kSiteAddressMargin.
[[nodiscard]] bool NeedsFreshAddresses(const model::Download& download, std::int64_t nowUnixMs);

// The failure an expired address gives: the server refuses it (403 Forbidden or 410 Gone).
[[nodiscard]] bool LooksLikeExpiredAddress(const TransferFailure& failure);

struct FreshAddresses {
    std::vector<std::uint32_t> changed;    // tracks with a new address, to store
    std::vector<std::uint32_t> restarted;  // of those, the ones whose bytes on disk were dropped
    // HLS tracks with their format's playlist address now in sourceUrl, still listing the old
    // pieces: their playlists are read again before the run, and KeepProgress decides what stays.
    std::vector<std::uint32_t> relist;
    std::optional<std::string> failure;  // a track's format is no longer offered; nothing changed
    // The site's other headers for the formats now (media::SiteFormat::headers); they replace the
    // download's, since a session's key may have changed with its addresses.
    std::vector<std::string> headers;
};

// Gives each track of `download` the address of its format in `video`. A whole-file track whose
// format now has another length than the one recorded, or that had none, starts again from its
// first byte, as does a piece list that no longer lists the same pieces (KeepProgress): bytes of
// two different files are never put together.
[[nodiscard]] FreshAddresses ApplyFreshAddresses(model::Download& download, const media::SiteVideo& video);

}  // namespace ldm::core
