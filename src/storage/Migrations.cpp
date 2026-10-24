#include "storage/Migrations.h"

#include <array>
#include <string>
#include <string_view>

namespace ldm::storage {

namespace {

struct Migration {
    int version;
    std::string_view sql;
};

// Append new migrations at the end. Never edit one that has shipped.
constexpr std::array kMigrations{
    Migration{1, R"sql(
CREATE TABLE categories (
  id            INTEGER PRIMARY KEY,
  name          TEXT NOT NULL UNIQUE COLLATE NOCASE,
  extensions    TEXT NOT NULL DEFAULT '',
  sites         TEXT NOT NULL DEFAULT '',
  folder        TEXT NOT NULL,
  remember_last INTEGER NOT NULL DEFAULT 0 CHECK (remember_last IN (0, 1)),
  builtin       INTEGER NOT NULL DEFAULT 0 CHECK (builtin IN (0, 1)),
  sort_order    INTEGER NOT NULL
) STRICT;

CREATE TABLE downloads (
  id                INTEGER PRIMARY KEY,
  public_id         TEXT NOT NULL UNIQUE,
  url               TEXT NOT NULL,
  final_url         TEXT,
  referer           TEXT,
  parent_page       TEXT,
  user_agent        TEXT,
  file_name         TEXT NOT NULL,
  save_dir          TEXT NOT NULL,
  partial_path      TEXT,
  category_id       INTEGER REFERENCES categories(id) ON DELETE SET NULL,
  description       TEXT NOT NULL DEFAULT '',
  status            TEXT NOT NULL CHECK (status IN
                      ('queued','downloading','stopping','paused','finalizing',
                       'completed','failed','deleting')),
  epoch             INTEGER NOT NULL DEFAULT 0,
  total_size        INTEGER CHECK (total_size >= 0),
  resumable         INTEGER CHECK (resumable IN (0, 1)),
  etag              TEXT,
  last_modified     TEXT,
  content_type      TEXT,
  max_connections   INTEGER CHECK (max_connections BETWEEN 1 AND 32),
  speed_limit_bps   INTEGER CHECK (speed_limit_bps > 0),
  login_user        TEXT,
  login_secret      BLOB,
  completion_action TEXT NOT NULL DEFAULT 'none'
                      CHECK (completion_action IN ('none','exit','shutdown','sleep','hibernate')),
  created_at        INTEGER NOT NULL,
  last_try_at       INTEGER,
  completed_at      INTEGER,
  last_result       TEXT,
  sort_order        INTEGER NOT NULL
) STRICT;
CREATE INDEX downloads_status ON downloads(status);
CREATE INDEX downloads_url ON downloads(url);

CREATE TABLE segments (
  download_id  INTEGER NOT NULL REFERENCES downloads(id) ON DELETE CASCADE,
  idx          INTEGER NOT NULL CHECK (idx >= 0),
  start_offset INTEGER NOT NULL CHECK (start_offset >= 0),
  end_offset   INTEGER CHECK (end_offset IS NULL OR end_offset >= start_offset),
  done_bytes   INTEGER NOT NULL DEFAULT 0 CHECK (done_bytes >= 0),
  PRIMARY KEY (download_id, idx)
) STRICT;

CREATE TABLE site_logins (
  id       INTEGER PRIMARY KEY,
  pattern  TEXT NOT NULL UNIQUE,
  username TEXT NOT NULL,
  secret   BLOB NOT NULL
) STRICT;

CREATE TABLE server_limits (
  pattern         TEXT PRIMARY KEY,
  max_connections INTEGER NOT NULL CHECK (max_connections BETWEEN 1 AND 32)
) STRICT;

CREATE TABLE settings (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
) STRICT;
)sql"},
    // Browser capture (M3): the captured request's cookies, DPAPI protected like login_secret.
    Migration{2, R"sql(
ALTER TABLE downloads ADD COLUMN cookie_secret BLOB;
)sql"},
    // Form (POST) downloads: the body, DPAPI protected, and its Content-Type.
    Migration{3, R"sql(
ALTER TABLE downloads ADD COLUMN post_secret BLOB;
ALTER TABLE downloads ADD COLUMN post_content_type TEXT;
)sql"},
    // Speculative adds whose info dialog was still open, removed at startup after a crash.
    Migration{4, R"sql(
ALTER TABLE downloads ADD COLUMN awaiting_confirmation INTEGER NOT NULL DEFAULT 0
  CHECK (awaiting_confirmation IN (0, 1));
)sql"},
    // Streamed video (HLS, DASH): the kind of download, its tracks with their pieces and keys,
    // and byte ranges per track for the tracks that are one file. Files keep track 0.
    Migration{5, R"sql(
ALTER TABLE downloads ADD COLUMN kind TEXT NOT NULL DEFAULT 'file' CHECK (kind IN ('file','hls','dash'));
ALTER TABLE downloads ADD COLUMN media_private_source INTEGER NOT NULL DEFAULT 0
  CHECK (media_private_source IN (0, 1));

CREATE TABLE segments_v5 (
  download_id  INTEGER NOT NULL REFERENCES downloads(id) ON DELETE CASCADE,
  track        INTEGER NOT NULL DEFAULT 0 CHECK (track >= 0),
  idx          INTEGER NOT NULL CHECK (idx >= 0),
  start_offset INTEGER NOT NULL CHECK (start_offset >= 0),
  end_offset   INTEGER CHECK (end_offset IS NULL OR end_offset >= start_offset),
  done_bytes   INTEGER NOT NULL DEFAULT 0 CHECK (done_bytes >= 0),
  PRIMARY KEY (download_id, track, idx)
) STRICT;
INSERT INTO segments_v5 (download_id, track, idx, start_offset, end_offset, done_bytes)
  SELECT download_id, 0, idx, start_offset, end_offset, done_bytes FROM segments;
DROP TABLE segments;
ALTER TABLE segments_v5 RENAME TO segments;

CREATE TABLE media_tracks (
  download_id INTEGER NOT NULL REFERENCES downloads(id) ON DELETE CASCADE,
  track       INTEGER NOT NULL CHECK (track >= 0),
  role        TEXT NOT NULL CHECK (role IN ('video','audio','subtitles')),
  source_url  TEXT NOT NULL,
  codecs      TEXT NOT NULL DEFAULT '',
  language    TEXT NOT NULL DEFAULT '',
  name        TEXT NOT NULL DEFAULT '',
  whole_file  INTEGER NOT NULL DEFAULT 0 CHECK (whole_file IN (0, 1)),
  PRIMARY KEY (download_id, track)
) STRICT;

CREATE TABLE media_keys (
  download_id INTEGER NOT NULL,
  track       INTEGER NOT NULL,
  idx         INTEGER NOT NULL CHECK (idx >= 0),
  url         TEXT NOT NULL DEFAULT '',
  value       BLOB CHECK (value IS NULL OR length(value) = 16),
  PRIMARY KEY (download_id, track, idx),
  FOREIGN KEY (download_id, track) REFERENCES media_tracks(download_id, track) ON DELETE CASCADE
) STRICT;

CREATE TABLE media_segments (
  download_id   INTEGER NOT NULL,
  track         INTEGER NOT NULL,
  idx           INTEGER NOT NULL CHECK (idx >= 0),
  kind          TEXT NOT NULL CHECK (kind IN ('init','media')),
  url           TEXT NOT NULL,
  range_offset  INTEGER CHECK (range_offset IS NULL OR range_offset >= 0),
  range_length  INTEGER CHECK (range_length IS NULL OR range_length > 0),
  duration_us   INTEGER NOT NULL DEFAULT 0 CHECK (duration_us >= 0),
  sequence      INTEGER NOT NULL DEFAULT 0,
  discontinuity INTEGER NOT NULL DEFAULT 0 CHECK (discontinuity IN (0, 1)),
  init_idx      INTEGER,
  key_idx       INTEGER,
  iv            BLOB CHECK (iv IS NULL OR length(iv) = 16),
  done          INTEGER NOT NULL DEFAULT 0 CHECK (done IN (0, 1)),
  bytes         INTEGER CHECK (bytes IS NULL OR bytes >= 0),
  PRIMARY KEY (download_id, track, idx),
  FOREIGN KEY (download_id, track) REFERENCES media_tracks(download_id, track) ON DELETE CASCADE
) STRICT;
)sql"},
    // Live HLS recordings: a track whose playlist is still read again while it downloads.
    Migration{6, R"sql(
ALTER TABLE media_tracks ADD COLUMN live INTEGER NOT NULL DEFAULT 0 CHECK (live IN (0, 1));
)sql"},
};

base::Result<int> CurrentVersion(Connection& connection)
{
    base::Status create =
        connection.Execute("CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL) STRICT");
    if (!create) {
        return create.GetError();
    }
    base::Result<Statement> query = connection.Prepare("SELECT version FROM schema_version");
    if (!query) {
        return query.GetError();
    }
    base::Result<bool> row = query.Value().Step();
    if (!row) {
        return row.GetError();
    }
    return row.Value() ? static_cast<int>(query.Value().Int64(0)) : 0;
}

base::Status Apply(Connection& connection, const Migration& migration, int fromVersion)
{
    base::Result<Transaction> transaction = Transaction::Begin(connection);
    if (!transaction) {
        return transaction.GetError();
    }
    base::Status schema = connection.Execute(migration.sql);
    if (!schema) {
        return base::MakeError(base::ErrorCode::Database, "migration " + std::to_string(migration.version) +
                                                              " failed: " + schema.GetError().message);
    }
    const std::string_view recordSql = fromVersion == 0 ? "INSERT INTO schema_version (version) VALUES (?1)"
                                                        : "UPDATE schema_version SET version = ?1";
    base::Result<Statement> record = connection.Prepare(recordSql);
    if (!record) {
        return record.GetError();
    }
    base::Status recorded = record.Value().Bind(1, migration.version).Run();
    if (!recorded) {
        return recorded;
    }
    return transaction.Value().Commit();
}

}  // namespace

int LatestSchemaVersion() noexcept
{
    return kMigrations.back().version;
}

base::Status Migrate(Connection& connection)
{
    return MigrateTo(connection, LatestSchemaVersion());
}

base::Status MigrateTo(Connection& connection, int target)
{
    base::Result<int> current = CurrentVersion(connection);
    if (!current) {
        return current.GetError();
    }
    int version = current.Value();
    if (version > LatestSchemaVersion()) {
        return base::MakeError(base::ErrorCode::Unsupported,
                               "the database was created by a newer version of Last Download Manager (schema " +
                                   std::to_string(version) + ")");
    }
    for (const Migration& migration : kMigrations) {
        if (migration.version <= version) {
            continue;
        }
        if (migration.version > target) {
            break;
        }
        base::Status applied = Apply(connection, migration, version);
        if (!applied) {
            return applied;
        }
        version = migration.version;
    }
    return {};
}

}  // namespace ldm::storage
