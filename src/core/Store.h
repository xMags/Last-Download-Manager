#pragma once

#include "base/Result.h"
#include "model/Category.h"
#include "model/ConnectionRules.h"
#include "model/Download.h"
#include "model/Settings.h"

#include <vector>

namespace ldm::core {

// Persistence port used by the coordinator. Implementations are used from one thread only.
// Every write method is atomic: it either fully applies or leaves the stored state unchanged.
class Store {
public:
    virtual ~Store() = default;

    [[nodiscard]] virtual base::Result<std::vector<model::Download>> LoadDownloads() = 0;
    // Inserts a new download with its segments and returns the assigned id. `download.id` is ignored.
    [[nodiscard]] virtual base::Result<model::DownloadId> InsertDownload(const model::Download& download) = 0;
    // Replaces the stored row and all its segments (a checkpoint).
    [[nodiscard]] virtual base::Status SaveDownload(const model::Download& download) = 0;
    [[nodiscard]] virtual base::Status DeleteDownload(model::DownloadId id) = 0;

    [[nodiscard]] virtual base::Result<std::vector<model::Category>> LoadCategories() = 0;
    [[nodiscard]] virtual base::Result<model::CategoryId> InsertCategory(const model::Category& category) = 0;
    [[nodiscard]] virtual base::Status SaveCategory(const model::Category& category) = 0;
    [[nodiscard]] virtual base::Status DeleteCategory(model::CategoryId id) = 0;

    [[nodiscard]] virtual base::Result<model::SettingsMap> LoadSettings() = 0;
    // Replaces every stored setting with the given map.
    [[nodiscard]] virtual base::Status SaveSettings(const model::SettingsMap& settings) = 0;

    [[nodiscard]] virtual base::Result<std::vector<model::SiteLogin>> LoadSiteLogins() = 0;
    [[nodiscard]] virtual base::Status ReplaceSiteLogins(const std::vector<model::SiteLogin>& logins) = 0;

    [[nodiscard]] virtual base::Result<std::vector<model::ServerLimit>> LoadServerLimits() = 0;
    [[nodiscard]] virtual base::Status ReplaceServerLimits(const std::vector<model::ServerLimit>& limits) = 0;
};

}  // namespace ldm::core
