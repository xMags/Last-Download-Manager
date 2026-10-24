#pragma once

#include "core/Store.h"
#include "storage/Sqlite.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

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
    base::Status MarkMediaPiecesDone(model::DownloadId id, std::span<const core::MediaPieceDone> pieces) override;
    base::Status ReplaceMediaTrack(model::DownloadId id, std::uint32_t track,
                                   const model::MediaTrack& replacement) override;
    base::Status AppendMediaTrack(model::DownloadId id, std::uint32_t track, const model::MediaTrack& grown,
                                  std::uint32_t firstKey, std::uint32_t firstPiece) override;
    base::Status DeleteDownload(model::DownloadId id) override;

    base::Result<std::vector<model::Category>> LoadCategories() override;
    base::Result<model::CategoryId> InsertCategory(const model::Category& category) override;
    base::Status SaveCategory(const model::Category& category) override;
    base::Status DeleteCategory(model::CategoryId id) override;

    base::Result<std::vector<model::Queue>> LoadQueues() override;
    base::Result<model::QueueId> InsertQueue(const model::Queue& queue) override;
    base::Status SaveQueue(const model::Queue& queue) override;
    base::Status DeleteQueue(model::QueueId id) override;

    base::Result<model::SettingsMap> LoadSettings() override;
    base::Status SaveSettings(const model::SettingsMap& settings) override;

    base::Result<std::vector<model::SiteLogin>> LoadSiteLogins() override;
    base::Status ReplaceSiteLogins(const std::vector<model::SiteLogin>& logins) override;

    base::Result<std::vector<model::ServerLimit>> LoadServerLimits() override;
    base::Status ReplaceServerLimits(const std::vector<model::ServerLimit>& limits) override;

private:
    explicit SqliteStore(Connection connection) noexcept : connection_(std::move(connection)) {}

    base::Status WriteSegments(const model::Download& download);
    base::Status WriteRanges(model::DownloadId id, std::uint32_t track, const std::vector<model::Segment>& ranges);
    base::Status WriteTrack(model::DownloadId id, std::uint32_t index, const model::MediaTrack& track);
    // The track's keys, or pieces, from index `first` on.
    base::Status WriteKeys(model::DownloadId id, std::uint32_t index, const model::MediaTrack& track, size_t first);
    base::Status WritePieces(model::DownloadId id, std::uint32_t index, const model::MediaTrack& track, size_t first);
    base::Status DeleteTrack(model::DownloadId id, std::uint32_t track);
    // Writes a queue's schedule into its row, or a new row when it has none; returns the row's id.
    base::Result<std::int64_t> WriteSchedule(std::optional<std::int64_t> id, const model::Schedule& schedule);
    base::Status LoadSegments(std::vector<model::Download>& downloads);
    base::Status LoadTracks(std::vector<model::Download>& downloads);

    Connection connection_;
};

}  // namespace ldm::storage
