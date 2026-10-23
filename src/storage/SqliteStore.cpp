#include "storage/SqliteStore.h"

#include "storage/Migrations.h"

#include <unordered_map>
#include <utility>

namespace ldm::storage {

namespace {

constexpr std::string_view kPragmas = R"sql(
PRAGMA journal_mode = WAL;
PRAGMA synchronous = FULL;
PRAGMA foreign_keys = ON;
PRAGMA locking_mode = EXCLUSIVE;
PRAGMA busy_timeout = 5000;
)sql";

// Column order shared by the SELECT and the row reader below.
constexpr std::string_view kSelectDownloads = R"sql(
SELECT id, public_id, url, final_url, referer, parent_page, user_agent, file_name, save_dir,
       partial_path, category_id, description, status, epoch, total_size, resumable, etag,
       last_modified, content_type, max_connections, speed_limit_bps, login_user, login_secret,
       completion_action, created_at, last_try_at, completed_at, last_result, sort_order, cookie_secret,
       post_secret, post_content_type, awaiting_confirmation
FROM downloads ORDER BY sort_order, id
)sql";

constexpr std::string_view kInsertDownload = R"sql(
INSERT INTO downloads (
  public_id, url, final_url, referer, parent_page, user_agent, file_name, save_dir,
  partial_path, category_id, description, status, epoch, total_size, resumable, etag,
  last_modified, content_type, max_connections, speed_limit_bps, login_user, login_secret,
  completion_action, created_at, last_try_at, completed_at, last_result, sort_order, cookie_secret,
  post_secret, post_content_type, awaiting_confirmation)
VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20,
        ?21, ?22, ?23, ?24, ?25, ?26, ?27, ?28, ?29, ?30, ?31, ?32)
)sql";

constexpr std::string_view kUpdateDownload = R"sql(
UPDATE downloads SET
  public_id = ?1, url = ?2, final_url = ?3, referer = ?4, parent_page = ?5, user_agent = ?6,
  file_name = ?7, save_dir = ?8, partial_path = ?9, category_id = ?10, description = ?11,
  status = ?12, epoch = ?13, total_size = ?14, resumable = ?15, etag = ?16, last_modified = ?17,
  content_type = ?18, max_connections = ?19, speed_limit_bps = ?20, login_user = ?21,
  login_secret = ?22, completion_action = ?23, created_at = ?24, last_try_at = ?25,
  completed_at = ?26, last_result = ?27, sort_order = ?28, cookie_secret = ?29, post_secret = ?30,
  post_content_type = ?31, awaiting_confirmation = ?32
WHERE id = ?33
)sql";

base::Error CorruptRow(std::string_view what)
{
    return base::MakeError(base::ErrorCode::Database, "invalid stored value: " + std::string(what));
}

std::optional<std::int64_t> ToOptionalId(const std::optional<model::CategoryId>& id)
{
    return id.has_value() ? std::optional<std::int64_t>(id->value) : std::nullopt;
}

std::optional<std::int64_t> ToOptionalInt(const std::optional<int>& value)
{
    return value.has_value() ? std::optional<std::int64_t>(*value) : std::nullopt;
}

std::optional<std::int64_t> ToOptionalBool(const std::optional<bool>& value)
{
    return value.has_value() ? std::optional<std::int64_t>(*value ? 1 : 0) : std::nullopt;
}

// Binds parameters ?1..?32 in the column order of kInsertDownload / kUpdateDownload.
void BindDownloadColumns(Statement& statement, const model::Download& d)
{
    statement.Bind(1, d.publicId)
        .Bind(2, d.url)
        .Bind(3, d.finalUrl)
        .Bind(4, d.referer)
        .Bind(5, d.parentPage)
        .Bind(6, d.userAgent)
        .Bind(7, d.fileName)
        .Bind(8, d.saveDir)
        .Bind(9, d.partialPath)
        .Bind(10, ToOptionalId(d.categoryId))
        .Bind(11, d.description)
        .Bind(12, model::ToString(d.status))
        .Bind(13, d.epoch)
        .Bind(14, d.totalSize)
        .Bind(15, ToOptionalBool(d.resumable))
        .Bind(16, d.validators.etag)
        .Bind(17, d.validators.lastModified)
        .Bind(18, d.contentType)
        .Bind(19, ToOptionalInt(d.maxConnections))
        .Bind(20, d.speedLimitBytesPerSecond)
        .Bind(21, d.loginUser);
    if (d.loginSecret.empty()) {
        statement.BindNull(22);
    } else {
        statement.Bind(22, std::span<const std::uint8_t>(d.loginSecret));
    }
    statement.Bind(23, model::ToString(d.completionAction))
        .Bind(24, d.createdAtMs)
        .Bind(25, d.lastTryAtMs)
        .Bind(26, d.completedAtMs)
        .Bind(27, d.lastResult)
        .Bind(28, d.sortOrder);
    if (d.cookieSecret.empty()) {
        statement.BindNull(29);
    } else {
        statement.Bind(29, std::span<const std::uint8_t>(d.cookieSecret));
    }
    if (d.postSecret.empty()) {
        statement.BindNull(30);
    } else {
        statement.Bind(30, std::span<const std::uint8_t>(d.postSecret));
    }
    statement.Bind(31, d.postContentType).Bind(32, std::int64_t{d.awaitingConfirmation ? 1 : 0});
}

base::Result<model::Download> ReadDownload(const Statement& row)
{
    model::Download d;
    d.id = model::DownloadId{row.Int64(0)};
    d.publicId = row.Text(1);
    d.url = row.Text(2);
    d.finalUrl = row.OptionalText(3);
    d.referer = row.OptionalText(4);
    d.parentPage = row.OptionalText(5);
    d.userAgent = row.OptionalText(6);
    d.fileName = row.Text(7);
    d.saveDir = row.Text(8);
    d.partialPath = row.OptionalText(9);
    if (const auto category = row.OptionalInt64(10)) {
        d.categoryId = model::CategoryId{*category};
    }
    d.description = row.Text(11);
    const auto status = model::ParseDownloadStatus(row.Text(12));
    if (!status) {
        return CorruptRow("downloads.status");
    }
    d.status = *status;
    d.epoch = row.Int64(13);
    d.totalSize = row.OptionalInt64(14);
    if (const auto resumable = row.OptionalInt64(15)) {
        d.resumable = *resumable != 0;
    }
    d.validators.etag = row.OptionalText(16);
    d.validators.lastModified = row.OptionalText(17);
    d.contentType = row.OptionalText(18);
    if (const auto connections = row.OptionalInt64(19)) {
        d.maxConnections = static_cast<int>(*connections);
    }
    d.speedLimitBytesPerSecond = row.OptionalInt64(20);
    d.loginUser = row.OptionalText(21);
    d.loginSecret = row.Blob(22);
    const auto action = model::ParseCompletionAction(row.Text(23));
    if (!action) {
        return CorruptRow("downloads.completion_action");
    }
    d.completionAction = *action;
    d.createdAtMs = row.Int64(24);
    d.lastTryAtMs = row.OptionalInt64(25);
    d.completedAtMs = row.OptionalInt64(26);
    d.lastResult = row.OptionalText(27);
    d.sortOrder = row.Int64(28);
    d.cookieSecret = row.Blob(29);
    d.postSecret = row.Blob(30);
    d.postContentType = row.OptionalText(31);
    d.awaitingConfirmation = row.Int64(32) != 0;
    return d;
}

}  // namespace

base::Result<SqliteStore> SqliteStore::Open(const std::string& path)
{
    base::Result<Connection> connection = Connection::Open(path);
    if (!connection) {
        return connection.GetError();
    }
    base::Status pragmas = connection.Value().Execute(kPragmas);
    if (!pragmas) {
        return pragmas.GetError();
    }
    base::Status migrated = Migrate(connection.Value());
    if (!migrated) {
        return migrated.GetError();
    }
    return SqliteStore(std::move(connection).Value());
}

// ---- downloads ----

base::Result<std::vector<model::Download>> SqliteStore::LoadDownloads()
{
    base::Result<Statement> query = connection_.Prepare(kSelectDownloads);
    if (!query) {
        return query.GetError();
    }
    std::vector<model::Download> downloads;
    for (;;) {
        base::Result<bool> row = query.Value().Step();
        if (!row) {
            return row.GetError();
        }
        if (!row.Value()) {
            break;
        }
        base::Result<model::Download> download = ReadDownload(query.Value());
        if (!download) {
            return download.GetError();
        }
        downloads.push_back(std::move(download).Value());
    }
    base::Status segments = LoadSegments(downloads);
    if (!segments) {
        return segments.GetError();
    }
    return downloads;
}

base::Status SqliteStore::LoadSegments(std::vector<model::Download>& downloads)
{
    std::unordered_map<model::DownloadId, model::Download*> byId;
    byId.reserve(downloads.size());
    for (model::Download& download : downloads) {
        byId.emplace(download.id, &download);
    }
    base::Result<Statement> query = connection_.Prepare(
        "SELECT download_id, start_offset, end_offset, done_bytes FROM segments ORDER BY download_id, idx");
    if (!query) {
        return query.GetError();
    }
    for (;;) {
        base::Result<bool> row = query.Value().Step();
        if (!row) {
            return row.GetError();
        }
        if (!row.Value()) {
            return {};
        }
        const Statement& r = query.Value();
        const auto owner = byId.find(model::DownloadId{r.Int64(0)});
        if (owner == byId.end()) {
            continue;  // unreachable with foreign keys on; ignore rather than fail the whole load
        }
        owner->second->segments.push_back(model::Segment{r.Int64(1), r.OptionalInt64(2), r.Int64(3)});
    }
}

base::Status SqliteStore::WriteSegments(const model::Download& download)
{
    base::Result<Statement> clear = connection_.Prepare("DELETE FROM segments WHERE download_id = ?1");
    if (!clear) {
        return clear.GetError();
    }
    base::Status cleared = clear.Value().Bind(1, download.id.value).Run();
    if (!cleared) {
        return cleared;
    }
    base::Result<Statement> insert = connection_.Prepare(
        "INSERT INTO segments (download_id, idx, start_offset, end_offset, done_bytes) VALUES (?1, ?2, ?3, ?4, ?5)");
    if (!insert) {
        return insert.GetError();
    }
    for (size_t index = 0; index < download.segments.size(); ++index) {
        const model::Segment& segment = download.segments[index];
        Statement& statement = insert.Value();
        statement.Reset();
        base::Status inserted = statement.Bind(1, download.id.value)
                                    .Bind(2, static_cast<std::int64_t>(index))
                                    .Bind(3, segment.start)
                                    .Bind(4, segment.end)
                                    .Bind(5, segment.done)
                                    .Run();
        if (!inserted) {
            return inserted;
        }
    }
    return {};
}

base::Result<model::DownloadId> SqliteStore::InsertDownload(const model::Download& download)
{
    base::Result<Transaction> transaction = Transaction::Begin(connection_);
    if (!transaction) {
        return transaction.GetError();
    }
    base::Result<Statement> insert = connection_.Prepare(kInsertDownload);
    if (!insert) {
        return insert.GetError();
    }
    BindDownloadColumns(insert.Value(), download);
    base::Status inserted = insert.Value().Run();
    if (!inserted) {
        return inserted.GetError();
    }
    model::Download stored = download;
    stored.id = model::DownloadId{connection_.LastInsertRowId()};
    base::Status segments = WriteSegments(stored);
    if (!segments) {
        return segments.GetError();
    }
    base::Status committed = transaction.Value().Commit();
    if (!committed) {
        return committed.GetError();
    }
    return stored.id;
}

base::Status SqliteStore::SaveDownload(const model::Download& download)
{
    base::Result<Transaction> transaction = Transaction::Begin(connection_);
    if (!transaction) {
        return transaction.GetError();
    }
    base::Result<Statement> update = connection_.Prepare(kUpdateDownload);
    if (!update) {
        return update.GetError();
    }
    BindDownloadColumns(update.Value(), download);
    update.Value().Bind(33, download.id.value);
    base::Status updated = update.Value().Run();
    if (!updated) {
        return updated;
    }
    if (connection_.ChangedRows() != 1) {
        return base::MakeError(base::ErrorCode::NotFound, "download does not exist");
    }
    base::Status segments = WriteSegments(download);
    if (!segments) {
        return segments;
    }
    return transaction.Value().Commit();
}

base::Status SqliteStore::DeleteDownload(model::DownloadId id)
{
    base::Result<Statement> remove = connection_.Prepare("DELETE FROM downloads WHERE id = ?1");
    if (!remove) {
        return remove.GetError();
    }
    return remove.Value().Bind(1, id.value).Run();
}

// ---- categories ----

base::Result<std::vector<model::Category>> SqliteStore::LoadCategories()
{
    base::Result<Statement> query = connection_.Prepare(
        "SELECT id, name, extensions, sites, folder, remember_last, builtin, sort_order FROM categories "
        "ORDER BY sort_order, id");
    if (!query) {
        return query.GetError();
    }
    std::vector<model::Category> categories;
    for (;;) {
        base::Result<bool> row = query.Value().Step();
        if (!row) {
            return row.GetError();
        }
        if (!row.Value()) {
            return categories;
        }
        const Statement& r = query.Value();
        categories.push_back(model::Category{model::CategoryId{r.Int64(0)}, r.Text(1), r.Text(2), r.Text(3),
                                             r.Text(4), r.Int64(5) != 0, r.Int64(6) != 0, r.Int64(7)});
    }
}

base::Result<model::CategoryId> SqliteStore::InsertCategory(const model::Category& category)
{
    base::Result<Statement> insert = connection_.Prepare(
        "INSERT INTO categories (name, extensions, sites, folder, remember_last, builtin, sort_order) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)");
    if (!insert) {
        return insert.GetError();
    }
    base::Status inserted = insert.Value()
                                .Bind(1, category.name)
                                .Bind(2, category.extensions)
                                .Bind(3, category.sites)
                                .Bind(4, category.folder)
                                .Bind(5, category.rememberLastFolder)
                                .Bind(6, category.builtin)
                                .Bind(7, category.sortOrder)
                                .Run();
    if (!inserted) {
        return inserted.GetError();
    }
    return model::CategoryId{connection_.LastInsertRowId()};
}

base::Status SqliteStore::SaveCategory(const model::Category& category)
{
    base::Result<Statement> update = connection_.Prepare(
        "UPDATE categories SET name = ?1, extensions = ?2, sites = ?3, folder = ?4, remember_last = ?5, "
        "builtin = ?6, sort_order = ?7 WHERE id = ?8");
    if (!update) {
        return update.GetError();
    }
    base::Status updated = update.Value()
                               .Bind(1, category.name)
                               .Bind(2, category.extensions)
                               .Bind(3, category.sites)
                               .Bind(4, category.folder)
                               .Bind(5, category.rememberLastFolder)
                               .Bind(6, category.builtin)
                               .Bind(7, category.sortOrder)
                               .Bind(8, category.id.value)
                               .Run();
    if (!updated) {
        return updated;
    }
    if (connection_.ChangedRows() != 1) {
        return base::MakeError(base::ErrorCode::NotFound, "category does not exist");
    }
    return {};
}

base::Status SqliteStore::DeleteCategory(model::CategoryId id)
{
    base::Result<Statement> remove = connection_.Prepare("DELETE FROM categories WHERE id = ?1");
    if (!remove) {
        return remove.GetError();
    }
    return remove.Value().Bind(1, id.value).Run();
}

// ---- settings ----

base::Result<model::SettingsMap> SqliteStore::LoadSettings()
{
    base::Result<Statement> query = connection_.Prepare("SELECT key, value FROM settings");
    if (!query) {
        return query.GetError();
    }
    model::SettingsMap settings;
    for (;;) {
        base::Result<bool> row = query.Value().Step();
        if (!row) {
            return row.GetError();
        }
        if (!row.Value()) {
            return settings;
        }
        settings.insert_or_assign(query.Value().Text(0), query.Value().Text(1));
    }
}

base::Status SqliteStore::SaveSettings(const model::SettingsMap& settings)
{
    base::Result<Transaction> transaction = Transaction::Begin(connection_);
    if (!transaction) {
        return transaction.GetError();
    }
    base::Status cleared = connection_.Execute("DELETE FROM settings");
    if (!cleared) {
        return cleared;
    }
    base::Result<Statement> insert = connection_.Prepare("INSERT INTO settings (key, value) VALUES (?1, ?2)");
    if (!insert) {
        return insert.GetError();
    }
    for (const auto& [key, value] : settings) {
        insert.Value().Reset();
        base::Status inserted = insert.Value().Bind(1, key).Bind(2, value).Run();
        if (!inserted) {
            return inserted;
        }
    }
    return transaction.Value().Commit();
}

// ---- connection rules ----

base::Result<std::vector<model::SiteLogin>> SqliteStore::LoadSiteLogins()
{
    base::Result<Statement> query =
        connection_.Prepare("SELECT id, pattern, username, secret FROM site_logins ORDER BY pattern");
    if (!query) {
        return query.GetError();
    }
    std::vector<model::SiteLogin> logins;
    for (;;) {
        base::Result<bool> row = query.Value().Step();
        if (!row) {
            return row.GetError();
        }
        if (!row.Value()) {
            return logins;
        }
        const Statement& r = query.Value();
        logins.push_back(model::SiteLogin{r.Int64(0), r.Text(1), r.Text(2), r.Blob(3)});
    }
}

base::Status SqliteStore::ReplaceSiteLogins(const std::vector<model::SiteLogin>& logins)
{
    base::Result<Transaction> transaction = Transaction::Begin(connection_);
    if (!transaction) {
        return transaction.GetError();
    }
    base::Status cleared = connection_.Execute("DELETE FROM site_logins");
    if (!cleared) {
        return cleared;
    }
    base::Result<Statement> insert =
        connection_.Prepare("INSERT INTO site_logins (pattern, username, secret) VALUES (?1, ?2, ?3)");
    if (!insert) {
        return insert.GetError();
    }
    for (const model::SiteLogin& login : logins) {
        insert.Value().Reset();
        base::Status inserted = insert.Value()
                                    .Bind(1, login.pattern)
                                    .Bind(2, login.username)
                                    .Bind(3, std::span<const std::uint8_t>(login.secret))
                                    .Run();
        if (!inserted) {
            return inserted;
        }
    }
    return transaction.Value().Commit();
}

base::Result<std::vector<model::ServerLimit>> SqliteStore::LoadServerLimits()
{
    base::Result<Statement> query =
        connection_.Prepare("SELECT pattern, max_connections FROM server_limits ORDER BY pattern");
    if (!query) {
        return query.GetError();
    }
    std::vector<model::ServerLimit> limits;
    for (;;) {
        base::Result<bool> row = query.Value().Step();
        if (!row) {
            return row.GetError();
        }
        if (!row.Value()) {
            return limits;
        }
        limits.push_back(
            model::ServerLimit{query.Value().Text(0), static_cast<int>(query.Value().Int64(1))});
    }
}

base::Status SqliteStore::ReplaceServerLimits(const std::vector<model::ServerLimit>& limits)
{
    base::Result<Transaction> transaction = Transaction::Begin(connection_);
    if (!transaction) {
        return transaction.GetError();
    }
    base::Status cleared = connection_.Execute("DELETE FROM server_limits");
    if (!cleared) {
        return cleared;
    }
    base::Result<Statement> insert =
        connection_.Prepare("INSERT INTO server_limits (pattern, max_connections) VALUES (?1, ?2)");
    if (!insert) {
        return insert.GetError();
    }
    for (const model::ServerLimit& limit : limits) {
        insert.Value().Reset();
        base::Status inserted = insert.Value().Bind(1, limit.pattern).Bind(2, limit.maxConnections).Run();
        if (!inserted) {
            return inserted;
        }
    }
    return transaction.Value().Commit();
}

}  // namespace ldm::storage
