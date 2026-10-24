#pragma once

#include "core/Commands.h"
#include "core/Ports.h"
#include "core/Snapshot.h"
#include "core/TransferEngine.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace ldm::core {

// Manifest fetches for the media panel (M3 design section 6), on the core thread beside the
// coordinator: each picks its proxy exactly as a download does, runs on the engine, and is
// answered to the front end as a FetchFinished notification.
//
// A proxy lookup is keyed by a DownloadId; fetches use negative ones (-fetchId), which no
// download has, so their late ProxyResolved answers can be told apart from downloads'.
class ManifestFetches {
public:
    ManifestFetches(TransferEngine& engine, ProxyResolver& proxy, CoreObserver& observer);

    // `userAgent` is Options' User-Agent, used when the command brings none.
    void Start(FetchManifestCommand command, const std::string& userAgent);
    // A ProxyResolved event for a negative id.
    void OnProxyResolved(model::DownloadId id, const std::optional<std::string>& proxy);
    void OnFinished(FetchFinished finished);

    [[nodiscard]] static bool IsFetchKey(model::DownloadId id) noexcept { return id.value < 0; }

private:
    TransferEngine& engine_;
    ProxyResolver& proxy_;
    CoreObserver& observer_;
    std::map<std::int64_t, FetchRequest> waitingForProxy_;  // by fetch id
};

}  // namespace ldm::core
