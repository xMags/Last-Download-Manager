#pragma once

#include "core/Store.h"
#include "storage/Sqlite.h"

#include <string>

namespace ldm::storage {

// SQLite implementation of the persistence port. Owned and used by the core thread only.
class SqliteStore final : public core::Store {
public:
    // Opens (creating if needed) and migrates the database. Holds an exclusive lock for its
    // lifetime, so a second process cannot open the same file. Pass ":memory:" for tests.
    [[nodiscard]] static base::Result<SqliteStore> Open(const std::string& path);

    SqliteStore(SqliteStore&&) noexcept = default;
    SqliteStore& operator=(SqliteStore&&) noexcept = default;

    base::Result<std::vector<model::Download>> LoadDownloads() override;
    base::Result<model::DownloadId> InsertDownload(const model::Download& download) override;
    base::Status SaveDownload(const model::Download& download) override;
    base::Status DeleteDownload(model::DownloadId id) override;

    base::Result<std::vector<model::Category>> LoadCategories() override;
    base::Result<model::CategoryId> InsertCategory(const model::Category& category) override;
    base::Status SaveCategory(const model::Category& category) override;
    base::Status DeleteCategory(model::CategoryId id) override;

    base::Result<model::SettingsMap> LoadSettings() override;
    base::Status SaveSettings(const model::SettingsMap& settings) override;

    base::Result<std::vector<model::SiteLogin>> LoadSiteLogins() override;
    base::Status ReplaceSiteLogins(const std::vector<model::SiteLogin>& logins) override;

    base::Result<std::vector<model::ServerLimit>> LoadServerLimits() override;
    base::Status ReplaceServerLimits(const std::vector<model::ServerLimit>& limits) override;

private:
    explicit SqliteStore(Connection connection) noexcept : connection_(std::move(connection)) {}

    base::Status WriteSegments(const model::Download& download);
    base::Status LoadSegments(std::vector<model::Download>& downloads);

    Connection connection_;
};

}  // namespace ldm::storage
