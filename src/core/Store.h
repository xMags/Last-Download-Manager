#pragma once

#include "base/Result.h"
#include "core/MediaProgress.h"
#include "model/Category.h"
#include "model/ConnectionRules.h"
#include "model/Download.h"
#include "model/Queue.h"
#include "model/Settings.h"

#include <cstdint>
#include <span>
#include <vector>

namespace ldm::core {

// Persistence port used by the coordinator. Implementations are used from one thread only.
// Every write method is atomic: it either fully applies or leaves the stored state unchanged.
class Store {
public:
    virtual ~Store() = default;

    [[nodiscard]] virtual base::Result<std::vector<model::Download>> LoadDownloads() = 0;
    // Inserts a new download with its segments, and a stream's tracks with their keys and pieces,
    // and returns the assigned id. `download.id` is ignored.
    [[nodiscard]] virtual base::Result<model::DownloadId> InsertDownload(const model::Download& download) = 0;
    // Replaces the stored row and all its byte ranges (a checkpoint): a file's segments, or the
    // ranges of a stream's whole-file tracks. A stream's tracks and pieces are not written here;
    // there can be many thousands of them, so they change through the two calls below.
    [[nodiscard]] virtual base::Status SaveDownload(const model::Download& download) = 0;
    // Marks pieces of a stream as on disk, with the sizes of their files.
    [[nodiscard]] virtual base::Status MarkMediaPiecesDone(model::DownloadId id,
                                                           std::span<const MediaPieceDone> pieces) = 0;
    // Replaces one track of a stream with its keys, pieces and ranges: its list was read, or its
    // progress reset.
    [[nodiscard]] virtual base::Status ReplaceMediaTrack(model::DownloadId id, std::uint32_t track,
                                                         const model::MediaTrack& replacement) = 0;
    // A live recording grew (M3 design section 6): stores the keys of `grown` from `firstKey` on
    // and its pieces from `firstPiece` on, which the stored track does not have yet, and its
    // `live` flag. A recording gains a piece every few seconds for hours, so it is not rewritten.
    [[nodiscard]] virtual base::Status AppendMediaTrack(model::DownloadId id, std::uint32_t track,
                                                        const model::MediaTrack& grown, std::uint32_t firstKey,
                                                        std::uint32_t firstPiece) = 0;
    [[nodiscard]] virtual base::Status DeleteDownload(model::DownloadId id) = 0;

    [[nodiscard]] virtual base::Result<std::vector<model::Category>> LoadCategories() = 0;
    [[nodiscard]] virtual base::Result<model::CategoryId> InsertCategory(const model::Category& category) = 0;
    [[nodiscard]] virtual base::Status SaveCategory(const model::Category& category) = 0;
    [[nodiscard]] virtual base::Status DeleteCategory(model::CategoryId id) = 0;

    // Queues come with their schedules, in their sort order.
    [[nodiscard]] virtual base::Result<std::vector<model::Queue>> LoadQueues() = 0;
    // Inserts a custom queue and its schedule and returns the assigned id. `queue.id` is ignored.
    [[nodiscard]] virtual base::Result<model::QueueId> InsertQueue(const model::Queue& queue) = 0;
    [[nodiscard]] virtual base::Status SaveQueue(const model::Queue& queue) = 0;
    // Deletes a custom queue and its schedule; its members stay as downloads outside any queue.
    // The built-in queues cannot be deleted.
    [[nodiscard]] virtual base::Status DeleteQueue(model::QueueId id) = 0;

    [[nodiscard]] virtual base::Result<model::SettingsMap> LoadSettings() = 0;
    // Replaces every stored setting with the given map.
    [[nodiscard]] virtual base::Status SaveSettings(const model::SettingsMap& settings) = 0;

    [[nodiscard]] virtual base::Result<std::vector<model::SiteLogin>> LoadSiteLogins() = 0;
    [[nodiscard]] virtual base::Status ReplaceSiteLogins(const std::vector<model::SiteLogin>& logins) = 0;

    [[nodiscard]] virtual base::Result<std::vector<model::ServerLimit>> LoadServerLimits() = 0;
    [[nodiscard]] virtual base::Status ReplaceServerLimits(const std::vector<model::ServerLimit>& limits) = 0;
};

}  // namespace ldm::core
