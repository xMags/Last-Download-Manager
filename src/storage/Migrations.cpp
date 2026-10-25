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
    // Pieces of a live recording that are left out: gone from the stream before they were fetched.
    Migration{7, R"sql(
ALTER TABLE media_segments ADD COLUMN skipped INTEGER NOT NULL DEFAULT 0 CHECK (skipped IN (0, 1));
)sql"},
    // Download queues and their schedules, with the two built-in queues. Whether a queue runs is
    // not stored: every queue is stopped when LDM starts.
    Migration{8, R"sql(
CREATE TABLE schedules (
  id                 INTEGER PRIMARY KEY,
  enabled            INTEGER NOT NULL DEFAULT 0 CHECK (enabled IN (0, 1)),
  mode               TEXT NOT NULL CHECK (mode IN ('once', 'daily')),
  start_minutes      INTEGER NOT NULL CHECK (start_minutes BETWEEN 0 AND 1439),
  start_day          INTEGER,
  weekdays           INTEGER NOT NULL DEFAULT 127 CHECK (weekdays BETWEEN 0 AND 127),
  repeat_minutes     INTEGER NOT NULL DEFAULT 0 CHECK (repeat_minutes >= 0),
  stop_enabled       INTEGER NOT NULL DEFAULT 0 CHECK (stop_enabled IN (0, 1)),
  stop_minutes       INTEGER NOT NULL DEFAULT 0 CHECK (stop_minutes BETWEEN 0 AND 1439),
  start_on_app_start INTEGER NOT NULL DEFAULT 0 CHECK (start_on_app_start IN (0, 1)),
  last_started_at    INTEGER
) STRICT;

CREATE TABLE queues (
  id                  INTEGER PRIMARY KEY,
  name                TEXT NOT NULL UNIQUE COLLATE NOCASE,
  kind                TEXT NOT NULL CHECK (kind IN ('main', 'sync', 'custom')),
  concurrent          INTEGER NOT NULL DEFAULT 1 CHECK (concurrent BETWEEN 1 AND 16),
  retries_per_file    INTEGER NOT NULL DEFAULT 3 CHECK (retries_per_file BETWEEN 0 AND 99),
  completion_action   TEXT NOT NULL DEFAULT 'none'
                        CHECK (completion_action IN ('none','exit','shutdown','sleep','hibernate')),
  force_terminate     INTEGER NOT NULL DEFAULT 0 CHECK (force_terminate IN (0, 1)),
  open_file_when_done TEXT NOT NULL DEFAULT '',
  schedule_id         INTEGER REFERENCES schedules(id) ON DELETE SET NULL,
  sort_order          INTEGER NOT NULL
) STRICT;

CREATE UNIQUE INDEX queues_builtin ON queues(kind) WHERE kind <> 'custom';

INSERT INTO queues (name, kind, sort_order) VALUES ('Main download queue', 'main', 0);
INSERT INTO queues (name, kind, sort_order) VALUES ('Synchronization queue', 'sync', 1);

ALTER TABLE downloads ADD COLUMN queue_id INTEGER REFERENCES queues(id) ON DELETE SET NULL;
ALTER TABLE downloads ADD COLUMN queue_position INTEGER NOT NULL DEFAULT 0;
ALTER TABLE downloads ADD COLUMN queue_failures INTEGER NOT NULL DEFAULT 0 CHECK (queue_failures >= 0);
CREATE INDEX downloads_queue ON downloads(queue_id, queue_position);
)sql"},
    // Site Grabber projects, templates and the pages and files they found (M4 design section
    // 6.1). A project's definition is one JSON document, read back as untrusted input. Its
    // downloads are ordinary rows, hidden from the main list until they are added to it.
    Migration{9, R"sql(
CREATE TABLE grabber_projects (
  id            INTEGER PRIMARY KEY,
  name          TEXT NOT NULL UNIQUE COLLATE NOCASE,
  is_template   INTEGER NOT NULL DEFAULT 0 CHECK (is_template IN (0, 1)),
  definition    TEXT NOT NULL,
  login_user    TEXT NOT NULL DEFAULT '',
  login_secret  BLOB,
  schedule_id   INTEGER REFERENCES schedules(id) ON DELETE SET NULL,
  schedule_mode TEXT NOT NULL DEFAULT 'explore'
                  CHECK (schedule_mode IN ('explore', 'explore_download', 'download_checked')),
  created_at    INTEGER NOT NULL,
  last_run_at   INTEGER
) STRICT;

CREATE TABLE grabber_items (
  project_id   INTEGER NOT NULL REFERENCES grabber_projects(id) ON DELETE CASCADE,
  id           INTEGER NOT NULL CHECK (id >= 1),
  url          TEXT NOT NULL,
  kind         TEXT NOT NULL CHECK (kind IN ('page', 'file')),
  parent_id    INTEGER,
  depth        INTEGER NOT NULL CHECK (depth >= 0),
  link_text    TEXT NOT NULL DEFAULT '',
  size         INTEGER CHECK (size >= 0),
  content_type TEXT NOT NULL DEFAULT '',
  checked      INTEGER NOT NULL DEFAULT 0 CHECK (checked IN (0, 1)),
  state        TEXT NOT NULL CHECK (state IN ('found', 'filtered', 'downloading', 'done', 'failed')),
  download_id  INTEGER REFERENCES downloads(id) ON DELETE SET NULL,
  local_path   TEXT NOT NULL DEFAULT '',
  PRIMARY KEY (project_id, id)
) STRICT;

CREATE INDEX grabber_items_download ON grabber_items(download_id) WHERE download_id IS NOT NULL;

ALTER TABLE downloads ADD COLUMN project_id INTEGER REFERENCES grabber_projects(id) ON DELETE SET NULL;
ALTER TABLE downloads ADD COLUMN listed INTEGER NOT NULL DEFAULT 1 CHECK (listed IN (0, 1));
)sql"},
    // Whether a download replaces the file at its destination when it completes, kept across
    // restarts (it was only in memory before).
    Migration{10, R"sql(
ALTER TABLE downloads ADD COLUMN replace_final INTEGER NOT NULL DEFAULT 0 CHECK (replace_final IN (0, 1));
)sql"},
    // Videos of supported sites: a DASH stream whose expiring track addresses are asked for again
    // by format, and checked against the length the site stated.
    Migration{11, R"sql(
ALTER TABLE downloads ADD COLUMN site_video INTEGER NOT NULL DEFAULT 0 CHECK (site_video IN (0, 1));
ALTER TABLE media_tracks ADD COLUMN site_format TEXT NOT NULL DEFAULT '';
ALTER TABLE media_tracks ADD COLUMN expected_bytes INTEGER CHECK (expected_bytes > 0);
)sql"},
    // Site videos added from a list (a playlist, a channel): the rule that picks their quality when
    // they first start, since their addresses would expire before a long queue reaches them.
    Migration{12, R"sql(
ALTER TABLE downloads ADD COLUMN site_quality TEXT NOT NULL DEFAULT '';
)sql"},
    // A site video's other request headers (a session's key), protected like its cookies and
    // removed with them once it completes.
    Migration{13, R"sql(
ALTER TABLE downloads ADD COLUMN site_header_secret BLOB;
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
