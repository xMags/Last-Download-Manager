#include "storage/SqliteStore.h"

#include "storage/Migrations.h"

#include <algorithm>
#include <limits>
#include <tuple>
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
       post_secret, post_content_type, awaiting_confirmation, kind, media_private_source, queue_id,
       queue_position, queue_failures, project_id, listed, replace_final
FROM downloads ORDER BY sort_order, id
)sql";

constexpr std::string_view kInsertDownload = R"sql(
INSERT INTO downloads (
  public_id, url, final_url, referer, parent_page, user_agent, file_name, save_dir,
  partial_path, category_id, description, status, epoch, total_size, resumable, etag,
  last_modified, content_type, max_connections, speed_limit_bps, login_user, login_secret,
  completion_action, created_at, last_try_at, completed_at, last_result, sort_order, cookie_secret,
  post_secret, post_content_type, awaiting_confirmation, kind, media_private_source, queue_id,
  queue_position, queue_failures, project_id, listed, replace_final)
VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20,
        ?21, ?22, ?23, ?24, ?25, ?26, ?27, ?28, ?29, ?30, ?31, ?32, ?33, ?34, ?35, ?36, ?37, ?38, ?39, ?40)
)sql";

constexpr std::string_view kUpdateDownload = R"sql(
UPDATE downloads SET
  public_id = ?1, url = ?2, final_url = ?3, referer = ?4, parent_page = ?5, user_agent = ?6,
  file_name = ?7, save_dir = ?8, partial_path = ?9, category_id = ?10, description = ?11,
  status = ?12, epoch = ?13, total_size = ?14, resumable = ?15, etag = ?16, last_modified = ?17,
  content_type = ?18, max_connections = ?19, speed_limit_bps = ?20, login_user = ?21,
  login_secret = ?22, completion_action = ?23, created_at = ?24, last_try_at = ?25,
  completed_at = ?26, last_result = ?27, sort_order = ?28, cookie_secret = ?29, post_secret = ?30,
  post_content_type = ?31, awaiting_confirmation = ?32, kind = ?33, media_private_source = ?34,
  queue_id = ?35, queue_position = ?36, queue_failures = ?37, project_id = ?38, listed = ?39,
  replace_final = ?40
WHERE id = ?41
)sql";

base::Error CorruptRow(std::string_view what)
{
    return base::MakeError(base::ErrorCode::Database, "invalid stored value: " + std::string(what));
}

std::optional<std::int64_t> ToOptionalId(const std::optional<model::CategoryId>& id)
{
    return id.has_value() ? std::optional<std::int64_t>(id->value) : std::nullopt;
}

std::optional<std::int64_t> ToOptionalId(const std::optional<model::QueueId>& id)
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

std::optional<std::int64_t> ToOptionalIndex(const std::optional<std::uint32_t>& value)
{
    return value.has_value() ? std::optional<std::int64_t>(*value) : std::nullopt;
}

void BindKey(Statement& statement, int index, const std::optional<model::AesKey>& key)
{
    if (key) {
        statement.Bind(index, std::span<const std::uint8_t>(*key));
    } else {
        statement.BindNull(index);
    }
}

std::optional<model::AesKey> ReadKey(const Statement& row, int column)
{
    const std::vector<std::uint8_t> bytes = row.Blob(column);
    if (bytes.size() != std::tuple_size_v<model::AesKey>) {
        return std::nullopt;  // NULL; the schema refuses any other length
    }
    model::AesKey key{};
    std::copy(bytes.begin(), bytes.end(), key.begin());
    return key;
}

std::optional<std::uint32_t> ReadIndex(const Statement& row, int column)
{
    const auto value = row.OptionalInt64(column);
    if (!value || *value < 0 || *value > std::int64_t{std::numeric_limits<std::uint32_t>::max()}) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*value);
}

using DownloadsById = std::unordered_map<model::DownloadId, model::Download*>;

DownloadsById IndexById(std::vector<model::Download>& downloads)
{
    DownloadsById byId;
    byId.reserve(downloads.size());
    for (model::Download& download : downloads) {
        byId.emplace(download.id, &download);
    }
    return byId;
}

model::Download* FindOwner(const DownloadsById& byId, std::int64_t id)
{
    const auto owner = byId.find(model::DownloadId{id});
    return owner == byId.end() ? nullptr : owner->second;
}

// The track of a row whose first two columns are download_id and track.
model::MediaTrack* FindTrack(const DownloadsById& byId, const Statement& row)
{
    model::Download* owner = FindOwner(byId, row.Int64(0));
    const std::int64_t index = row.Int64(1);
    if (owner == nullptr || index < 0 || static_cast<std::uint64_t>(index) >= owner->tracks.size()) {
        return nullptr;
    }
    return &owner->tracks[static_cast<size_t>(index)];
}

// The references inside a track point at rows that exist: LDM wrote them, and checks them on the
// way back rather than trusting the file.
bool TrackIsConsistent(const model::MediaTrack& track)
{
    for (const model::MediaPiece& piece : track.pieces) {
        if (piece.init && (*piece.init >= track.pieces.size() ||
                           track.pieces[*piece.init].kind != model::MediaPiece::Kind::Init)) {
            return false;
        }
        if (piece.key && *piece.key >= track.keys.size()) {
            return false;
        }
        if (piece.rangeOffset.has_value() != piece.rangeLength.has_value()) {
            return false;
        }
    }
    return !track.wholeFile || track.pieces.size() <= 1;
}

// Binds parameters ?1..?40 in the column order of kInsertDownload / kUpdateDownload.
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
    statement.Bind(31, d.postContentType)
        .Bind(32, std::int64_t{d.awaitingConfirmation ? 1 : 0})
        .Bind(33, model::ToString(d.kind))
        .Bind(34, std::int64_t{d.mediaFromPrivateNetwork ? 1 : 0})
        .Bind(35, ToOptionalId(d.queueId))
        .Bind(36, d.queuePosition)
        .Bind(37, d.queueFailures)
        .Bind(38, d.projectId ? std::optional<std::int64_t>(d.projectId->value) : std::nullopt)
        .Bind(39, d.listed)
        .Bind(40, d.replaceFinalFile);
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
    const auto kind = model::ParseDownloadKind(row.Text(33));
    if (!kind) {
        return CorruptRow("downloads.kind");
    }
    d.kind = *kind;
    d.mediaFromPrivateNetwork = row.Int64(34) != 0;
    if (const auto queue = row.OptionalInt64(35)) {
        d.queueId = model::QueueId{*queue};
    }
    d.queuePosition = row.Int64(36);
    const std::int64_t failures = row.Int64(37);
    if (failures < 0 || failures > std::numeric_limits<int>::max()) {
        return CorruptRow("downloads.queue_failures");
    }
    d.queueFailures = static_cast<int>(failures);
    if (const auto project = row.OptionalInt64(38)) {
        d.projectId = model::GrabberProjectId{*project};
    }
    d.listed = row.Int64(39) != 0;
    d.replaceFinalFile = row.Int64(40) != 0;
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
    base::Status tracks = LoadTracks(downloads);
    if (!tracks) {
        return tracks.GetError();
    }
    base::Status segments = LoadSegments(downloads);
    if (!segments) {
        return segments.GetError();
    }
    return downloads;
}

base::Status SqliteStore::LoadSegments(std::vector<model::Download>& downloads)
{
    const DownloadsById byId = IndexById(downloads);
    base::Result<Statement> query = connection_.Prepare(
        "SELECT download_id, track, start_offset, end_offset, done_bytes FROM segments "
        "ORDER BY download_id, track, idx");
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
        model::Download* owner = FindOwner(byId, r.Int64(0));
        if (owner == nullptr) {
            continue;  // unreachable with foreign keys on; ignore rather than fail the whole load
        }
        const model::Segment segment{r.Int64(2), r.OptionalInt64(3), r.Int64(4)};
        if (!owner->IsStream()) {
            if (r.Int64(1) != 0) {
                return CorruptRow("segments.track");
            }
            owner->segments.push_back(segment);
            continue;
        }
        model::MediaTrack* track = FindTrack(byId, r);
        if (track == nullptr || !track->wholeFile) {
            return CorruptRow("segments.track");
        }
        track->ranges.push_back(segment);
    }
}

base::Status SqliteStore::LoadTracks(std::vector<model::Download>& downloads)
{
    const DownloadsById byId = IndexById(downloads);
    base::Result<Statement> tracks = connection_.Prepare(
        "SELECT download_id, track, role, source_url, codecs, language, name, whole_file, live FROM media_tracks "
        "ORDER BY download_id, track");
    if (!tracks) {
        return tracks.GetError();
    }
    for (;;) {
        base::Result<bool> row = tracks.Value().Step();
        if (!row) {
            return row.GetError();
        }
        if (!row.Value()) {
            break;
        }
        const Statement& r = tracks.Value();
        model::Download* owner = FindOwner(byId, r.Int64(0));
        if (owner == nullptr) {
            continue;
        }
        // Rows come in index order, so each one is the next track of its download.
        const auto role = model::ParseTrackRole(r.Text(2));
        if (!role || r.Int64(1) != static_cast<std::int64_t>(owner->tracks.size())) {
            return CorruptRow("media_tracks");
        }
        model::MediaTrack track;
        track.role = *role;
        track.sourceUrl = r.Text(3);
        track.codecs = r.Text(4);
        track.language = r.Text(5);
        track.name = r.Text(6);
        track.wholeFile = r.Int64(7) != 0;
        track.live = r.Int64(8) != 0;
        owner->tracks.push_back(std::move(track));
    }

    base::Result<Statement> keys = connection_.Prepare(
        "SELECT download_id, track, idx, url, value FROM media_keys ORDER BY download_id, track, idx");
    if (!keys) {
        return keys.GetError();
    }
    for (;;) {
        base::Result<bool> row = keys.Value().Step();
        if (!row) {
            return row.GetError();
        }
        if (!row.Value()) {
            break;
        }
        const Statement& r = keys.Value();
        model::MediaTrack* track = FindTrack(byId, r);
        if (track == nullptr) {
            continue;
        }
        if (r.Int64(2) != static_cast<std::int64_t>(track->keys.size())) {
            return CorruptRow("media_keys.idx");
        }
        track->keys.push_back(model::MediaKey{r.Text(3), ReadKey(r, 4)});
    }

    base::Result<Statement> pieces = connection_.Prepare(
        "SELECT download_id, track, idx, kind, url, range_offset, range_length, duration_us, sequence, "
        "discontinuity, init_idx, key_idx, iv, done, bytes, skipped FROM media_segments "
        "ORDER BY download_id, track, idx");
    if (!pieces) {
        return pieces.GetError();
    }
    for (;;) {
        base::Result<bool> row = pieces.Value().Step();
        if (!row) {
            return row.GetError();
        }
        if (!row.Value()) {
            break;
        }
        const Statement& r = pieces.Value();
        model::MediaTrack* track = FindTrack(byId, r);
        if (track == nullptr) {
            continue;
        }
        const std::string kind = r.Text(3);
        if (r.Int64(2) != static_cast<std::int64_t>(track->pieces.size()) || (kind != "init" && kind != "media")) {
            return CorruptRow("media_segments");
        }
        model::MediaPiece piece;
        piece.kind = kind == "init" ? model::MediaPiece::Kind::Init : model::MediaPiece::Kind::Media;
        piece.url = r.Text(4);
        piece.rangeOffset = r.OptionalInt64(5);
        piece.rangeLength = r.OptionalInt64(6);
        piece.durationUs = r.Int64(7);
        piece.sequence = static_cast<std::uint64_t>(r.Int64(8));  // stored as its two's complement
        piece.discontinuity = r.Int64(9) != 0;
        piece.init = ReadIndex(r, 10);
        piece.key = ReadIndex(r, 11);
        piece.iv = ReadKey(r, 12);
        piece.done = r.Int64(13) != 0;
        piece.bytes = r.OptionalInt64(14);
        piece.skipped = r.Int64(15) != 0;
        track->pieces.push_back(std::move(piece));
    }

    for (const model::Download& download : downloads) {
        for (const model::MediaTrack& track : download.tracks) {
            if (!TrackIsConsistent(track)) {
                return CorruptRow("media_segments references");
            }
        }
    }
    return {};
}

base::Status SqliteStore::WriteRanges(model::DownloadId id, std::uint32_t track,
                                      const std::vector<model::Segment>& ranges)
{
    base::Result<Statement> clear = connection_.Prepare("DELETE FROM segments WHERE download_id = ?1 AND track = ?2");
    if (!clear) {
        return clear.GetError();
    }
    base::Status cleared = clear.Value().Bind(1, id.value).Bind(2, std::int64_t{track}).Run();
    if (!cleared) {
        return cleared;
    }
    base::Result<Statement> insert = connection_.Prepare(
        "INSERT INTO segments (download_id, track, idx, start_offset, end_offset, done_bytes) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
    if (!insert) {
        return insert.GetError();
    }
    for (size_t index = 0; index < ranges.size(); ++index) {
        const model::Segment& segment = ranges[index];
        Statement& statement = insert.Value();
        statement.Reset();
        base::Status inserted = statement.Bind(1, id.value)
                                    .Bind(2, std::int64_t{track})
                                    .Bind(3, static_cast<std::int64_t>(index))
                                    .Bind(4, segment.start)
                                    .Bind(5, segment.end)
                                    .Bind(6, segment.done)
                                    .Run();
        if (!inserted) {
            return inserted;
        }
    }
    return {};
}

base::Status SqliteStore::WriteSegments(const model::Download& download)
{
    if (!download.IsStream()) {
        return WriteRanges(download.id, 0, download.segments);
    }
    for (size_t index = 0; index < download.tracks.size(); ++index) {
        const model::MediaTrack& track = download.tracks[index];
        if (!track.wholeFile) {
            continue;
        }
        base::Status written = WriteRanges(download.id, static_cast<std::uint32_t>(index), track.ranges);
        if (!written) {
            return written;
        }
    }
    return {};
}

base::Status SqliteStore::DeleteTrack(model::DownloadId id, std::uint32_t track)
{
    // Keys and pieces go with the track row (ON DELETE CASCADE); ranges hang off the download.
    base::Result<Statement> remove =
        connection_.Prepare("DELETE FROM media_tracks WHERE download_id = ?1 AND track = ?2");
    if (!remove) {
        return remove.GetError();
    }
    base::Status removed = remove.Value().Bind(1, id.value).Bind(2, std::int64_t{track}).Run();
    if (!removed) {
        return removed;
    }
    return WriteRanges(id, track, {});
}

base::Status SqliteStore::WriteTrack(model::DownloadId id, std::uint32_t index, const model::MediaTrack& track)
{
    base::Result<Statement> insertTrack = connection_.Prepare(
        "INSERT INTO media_tracks (download_id, track, role, source_url, codecs, language, name, whole_file, live) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)");
    if (!insertTrack) {
        return insertTrack.GetError();
    }
    base::Status inserted = insertTrack.Value()
                                .Bind(1, id.value)
                                .Bind(2, std::int64_t{index})
                                .Bind(3, model::ToString(track.role))
                                .Bind(4, track.sourceUrl)
                                .Bind(5, track.codecs)
                                .Bind(6, track.language)
                                .Bind(7, track.name)
                                .Bind(8, track.wholeFile)
                                .Bind(9, track.live)
                                .Run();
    if (!inserted) {
        return inserted;
    }
    base::Status keys = WriteKeys(id, index, track, 0);
    if (!keys) {
        return keys;
    }
    base::Status pieces = WritePieces(id, index, track, 0);
    if (!pieces) {
        return pieces;
    }
    return track.wholeFile ? WriteRanges(id, index, track.ranges) : base::Status{};
}

base::Status SqliteStore::WriteKeys(model::DownloadId id, std::uint32_t index, const model::MediaTrack& track,
                                    size_t first)
{
    base::Result<Statement> insertKey = connection_.Prepare(
        "INSERT INTO media_keys (download_id, track, idx, url, value) VALUES (?1, ?2, ?3, ?4, ?5)");
    if (!insertKey) {
        return insertKey.GetError();
    }
    for (size_t k = first; k < track.keys.size(); ++k) {
        Statement& statement = insertKey.Value();
        statement.Reset();
        statement.Bind(1, id.value)
            .Bind(2, std::int64_t{index})
            .Bind(3, static_cast<std::int64_t>(k))
            .Bind(4, track.keys[k].url);
        BindKey(statement, 5, track.keys[k].value);
        base::Status keyInserted = statement.Run();
        if (!keyInserted) {
            return keyInserted;
        }
    }
    return {};
}

base::Status SqliteStore::WritePieces(model::DownloadId id, std::uint32_t index, const model::MediaTrack& track,
                                      size_t first)
{
    base::Result<Statement> insertPiece = connection_.Prepare(
        "INSERT INTO media_segments (download_id, track, idx, kind, url, range_offset, range_length, duration_us, "
        "sequence, discontinuity, init_idx, key_idx, iv, done, bytes, skipped) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16)");
    if (!insertPiece) {
        return insertPiece.GetError();
    }
    for (size_t p = first; p < track.pieces.size(); ++p) {
        const model::MediaPiece& piece = track.pieces[p];
        Statement& statement = insertPiece.Value();
        statement.Reset();
        statement.Bind(1, id.value)
            .Bind(2, std::int64_t{index})
            .Bind(3, static_cast<std::int64_t>(p))
            .Bind(4, piece.kind == model::MediaPiece::Kind::Init ? "init" : "media")
            .Bind(5, piece.url)
            .Bind(6, piece.rangeOffset)
            .Bind(7, piece.rangeLength)
            .Bind(8, piece.durationUs)
            .Bind(9, static_cast<std::int64_t>(piece.sequence))
            .Bind(10, piece.discontinuity)
            .Bind(11, ToOptionalIndex(piece.init))
            .Bind(12, ToOptionalIndex(piece.key));
        BindKey(statement, 13, piece.iv);
        statement.Bind(14, piece.done).Bind(15, piece.bytes).Bind(16, piece.skipped);
        base::Status pieceInserted = statement.Run();
        if (!pieceInserted) {
            return pieceInserted;
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
    for (size_t index = 0; index < stored.tracks.size(); ++index) {
        base::Status track = WriteTrack(stored.id, static_cast<std::uint32_t>(index), stored.tracks[index]);
        if (!track) {
            return track.GetError();
        }
    }
    if (!stored.IsStream()) {
        base::Status segments = WriteSegments(stored);  // a stream's ranges were written with its tracks
        if (!segments) {
            return segments.GetError();
        }
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
    update.Value().Bind(41, download.id.value);
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

base::Status SqliteStore::MarkMediaPiecesDone(model::DownloadId id, std::span<const core::MediaPieceDone> pieces)
{
    base::Result<Transaction> transaction = Transaction::Begin(connection_);
    if (!transaction) {
        return transaction.GetError();
    }
    base::Result<Statement> update = connection_.Prepare(
        "UPDATE media_segments SET done = 1, bytes = ?4 WHERE download_id = ?1 AND track = ?2 AND idx = ?3");
    if (!update) {
        return update.GetError();
    }
    for (const core::MediaPieceDone& piece : pieces) {
        Statement& statement = update.Value();
        statement.Reset();
        base::Status updated = statement.Bind(1, id.value)
                                   .Bind(2, std::int64_t{piece.track})
                                   .Bind(3, std::int64_t{piece.index})
                                   .Bind(4, piece.bytes)
                                   .Run();
        if (!updated) {
            return updated;
        }
        if (connection_.ChangedRows() != 1) {
            return base::MakeError(base::ErrorCode::NotFound, "media segment does not exist");
        }
    }
    return transaction.Value().Commit();
}

base::Status SqliteStore::ReplaceMediaTrack(model::DownloadId id, std::uint32_t track,
                                            const model::MediaTrack& replacement)
{
    base::Result<Transaction> transaction = Transaction::Begin(connection_);
    if (!transaction) {
        return transaction.GetError();
    }
    base::Status removed = DeleteTrack(id, track);
    if (!removed) {
        return removed;
    }
    base::Status written = WriteTrack(id, track, replacement);
    if (!written) {
        return written;
    }
    return transaction.Value().Commit();
}

base::Status SqliteStore::AppendMediaTrack(model::DownloadId id, std::uint32_t track, const model::MediaTrack& grown,
                                           std::uint32_t firstKey, std::uint32_t firstPiece)
{
    if (firstKey > grown.keys.size() || firstPiece > grown.pieces.size()) {
        return base::MakeError(base::ErrorCode::InvalidArgument, "the appended part starts past the end of the track");
    }
    base::Result<Transaction> transaction = Transaction::Begin(connection_);
    if (!transaction) {
        return transaction.GetError();
    }
    base::Result<Statement> update =
        connection_.Prepare("UPDATE media_tracks SET live = ?3 WHERE download_id = ?1 AND track = ?2");
    if (!update) {
        return update.GetError();
    }
    base::Status updated = update.Value().Bind(1, id.value).Bind(2, std::int64_t{track}).Bind(3, grown.live).Run();
    if (!updated) {
        return updated;
    }
    if (connection_.ChangedRows() != 1) {
        return base::MakeError(base::ErrorCode::NotFound, "media track does not exist");
    }
    base::Status keys = WriteKeys(id, track, grown, firstKey);
    if (!keys) {
        return keys;
    }
    base::Status pieces = WritePieces(id, track, grown, firstPiece);
    if (!pieces) {
        return pieces;
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

// ---- queues ----

namespace {

// Column order shared by the SELECT and ReadQueue: the queue, then its schedule, if any.
constexpr std::string_view kSelectQueues = R"sql(
SELECT q.id, q.name, q.kind, q.concurrent, q.retries_per_file, q.completion_action, q.force_terminate,
       q.open_file_when_done, q.sort_order, s.id, s.enabled, s.mode, s.start_minutes, s.start_day, s.weekdays,
       s.repeat_minutes, s.stop_enabled, s.stop_minutes, s.start_on_app_start, s.last_started_at
FROM queues q LEFT JOIN schedules s ON s.id = q.schedule_id
ORDER BY q.sort_order, q.id
)sql";

base::Result<model::Schedule> ReadSchedule(const Statement& row, int first)
{
    model::Schedule schedule;
    if (!row.OptionalInt64(first)) {
        return schedule;  // a queue without a schedule row has the default, switched off
    }
    schedule.enabled = row.Int64(first + 1) != 0;
    const auto mode = model::ParseScheduleMode(row.Text(first + 2));
    if (!mode) {
        return CorruptRow("schedules.mode");
    }
    schedule.mode = *mode;
    // The schema's checks keep these within their ranges.
    schedule.startMinutes = static_cast<int>(row.Int64(first + 3));
    schedule.startDay = row.OptionalInt64(first + 4);
    schedule.weekdays = static_cast<std::uint8_t>(row.Int64(first + 5));
    const std::int64_t repeat = row.Int64(first + 6);
    if (repeat > std::numeric_limits<int>::max()) {
        return CorruptRow("schedules.repeat_minutes");
    }
    schedule.repeatMinutes = static_cast<int>(repeat);
    schedule.stopEnabled = row.Int64(first + 7) != 0;
    schedule.stopMinutes = static_cast<int>(row.Int64(first + 8));
    schedule.startOnAppStart = row.Int64(first + 9) != 0;
    schedule.lastStartedAtMs = row.OptionalInt64(first + 10);
    return schedule;
}

base::Result<model::Queue> ReadQueue(const Statement& row)
{
    model::Queue queue;
    queue.id = model::QueueId{row.Int64(0)};
    queue.name = row.Text(1);
    const auto kind = model::ParseQueueKind(row.Text(2));
    if (!kind) {
        return CorruptRow("queues.kind");
    }
    queue.kind = *kind;
    queue.concurrent = static_cast<int>(row.Int64(3));
    queue.retriesPerFile = static_cast<int>(row.Int64(4));
    const auto action = model::ParseCompletionAction(row.Text(5));
    if (!action) {
        return CorruptRow("queues.completion_action");
    }
    queue.completionAction = *action;
    queue.forceTerminate = row.Int64(6) != 0;
    queue.openFileWhenDone = row.Text(7);
    queue.sortOrder = row.Int64(8);
    base::Result<model::Schedule> schedule = ReadSchedule(row, 9);
    if (!schedule) {
        return schedule.GetError();
    }
    queue.schedule = std::move(schedule).Value();
    return queue;
}

// Binds ?2..?10 of the queue columns that can change; the caller binds ?1.
void BindQueueColumns(Statement& statement, const model::Queue& queue, std::int64_t scheduleId)
{
    statement.Bind(2, queue.name)
        .Bind(3, queue.concurrent)
        .Bind(4, queue.retriesPerFile)
        .Bind(5, model::ToString(queue.completionAction))
        .Bind(6, queue.forceTerminate)
        .Bind(7, queue.openFileWhenDone)
        .Bind(8, scheduleId)
        .Bind(9, queue.sortOrder);
}

struct StoredQueue {
    model::QueueKind kind = model::QueueKind::Custom;
    std::optional<std::int64_t> scheduleId;
};

base::Result<StoredQueue> FindQueue(Connection& connection, model::QueueId id)
{
    base::Result<Statement> query = connection.Prepare("SELECT kind, schedule_id FROM queues WHERE id = ?1");
    if (!query) {
        return query.GetError();
    }
    query.Value().Bind(1, id.value);
    base::Result<bool> row = query.Value().Step();
    if (!row) {
        return row.GetError();
    }
    if (!row.Value()) {
        return base::MakeError(base::ErrorCode::NotFound, "queue does not exist");
    }
    const auto kind = model::ParseQueueKind(query.Value().Text(0));
    if (!kind) {
        return CorruptRow("queues.kind");
    }
    return StoredQueue{*kind, query.Value().OptionalInt64(1)};
}

}  // namespace

base::Result<std::int64_t> SqliteStore::WriteSchedule(std::optional<std::int64_t> id, const model::Schedule& schedule)
{
    const auto bind = [&schedule](Statement& statement) {
        statement.Bind(1, schedule.enabled)
            .Bind(2, model::ToString(schedule.mode))
            .Bind(3, schedule.startMinutes)
            .Bind(4, schedule.startDay)
            .Bind(5, std::int64_t{schedule.weekdays})
            .Bind(6, schedule.repeatMinutes)
            .Bind(7, schedule.stopEnabled)
            .Bind(8, schedule.stopMinutes)
            .Bind(9, schedule.startOnAppStart)
            .Bind(10, schedule.lastStartedAtMs);
    };
    if (id) {
        base::Result<Statement> update = connection_.Prepare(
            "UPDATE schedules SET enabled = ?1, mode = ?2, start_minutes = ?3, start_day = ?4, weekdays = ?5, "
            "repeat_minutes = ?6, stop_enabled = ?7, stop_minutes = ?8, start_on_app_start = ?9, "
            "last_started_at = ?10 WHERE id = ?11");
        if (!update) {
            return update.GetError();
        }
        bind(update.Value());
        base::Status updated = update.Value().Bind(11, *id).Run();
        if (!updated) {
            return updated.GetError();
        }
        if (connection_.ChangedRows() == 1) {
            return *id;
        }
    }
    base::Result<Statement> insert = connection_.Prepare(
        "INSERT INTO schedules (enabled, mode, start_minutes, start_day, weekdays, repeat_minutes, stop_enabled, "
        "stop_minutes, start_on_app_start, last_started_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)");
    if (!insert) {
        return insert.GetError();
    }
    bind(insert.Value());
    base::Status inserted = insert.Value().Run();
    if (!inserted) {
        return inserted.GetError();
    }
    return connection_.LastInsertRowId();
}

base::Result<std::vector<model::Queue>> SqliteStore::LoadQueues()
{
    base::Result<Statement> query = connection_.Prepare(kSelectQueues);
    if (!query) {
        return query.GetError();
    }
    std::vector<model::Queue> queues;
    for (;;) {
        base::Result<bool> row = query.Value().Step();
        if (!row) {
            return row.GetError();
        }
        if (!row.Value()) {
            return queues;
        }
        base::Result<model::Queue> queue = ReadQueue(query.Value());
        if (!queue) {
            return queue.GetError();
        }
        queues.push_back(std::move(queue).Value());
    }
}

base::Result<model::QueueId> SqliteStore::InsertQueue(const model::Queue& queue)
{
    if (queue.kind != model::QueueKind::Custom) {
        return base::MakeError(base::ErrorCode::InvalidArgument, "only custom queues can be added");
    }
    base::Result<Transaction> transaction = Transaction::Begin(connection_);
    if (!transaction) {
        return transaction.GetError();
    }
    base::Result<std::int64_t> schedule = WriteSchedule(std::nullopt, queue.schedule);
    if (!schedule) {
        return schedule.GetError();
    }
    base::Result<Statement> insert = connection_.Prepare(
        "INSERT INTO queues (kind, name, concurrent, retries_per_file, completion_action, force_terminate, "
        "open_file_when_done, schedule_id, sort_order) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)");
    if (!insert) {
        return insert.GetError();
    }
    insert.Value().Bind(1, model::ToString(queue.kind));
    BindQueueColumns(insert.Value(), queue, schedule.Value());
    base::Status inserted = insert.Value().Run();
    if (!inserted) {
        return inserted.GetError();
    }
    const model::QueueId id{connection_.LastInsertRowId()};
    base::Status committed = transaction.Value().Commit();
    if (!committed) {
        return committed.GetError();
    }
    return id;
}

base::Status SqliteStore::SaveQueue(const model::Queue& queue)
{
    base::Result<Transaction> transaction = Transaction::Begin(connection_);
    if (!transaction) {
        return transaction.GetError();
    }
    base::Result<StoredQueue> stored = FindQueue(connection_, queue.id);
    if (!stored) {
        return stored.GetError();
    }
    base::Result<std::int64_t> schedule = WriteSchedule(stored.Value().scheduleId, queue.schedule);
    if (!schedule) {
        return schedule.GetError();
    }
    // The kind never changes: a built-in queue stays one.
    base::Result<Statement> update = connection_.Prepare(
        "UPDATE queues SET name = ?2, concurrent = ?3, retries_per_file = ?4, completion_action = ?5, "
        "force_terminate = ?6, open_file_when_done = ?7, schedule_id = ?8, sort_order = ?9 WHERE id = ?1");
    if (!update) {
        return update.GetError();
    }
    update.Value().Bind(1, queue.id.value);
    BindQueueColumns(update.Value(), queue, schedule.Value());
    base::Status updated = update.Value().Run();
    if (!updated) {
        return updated;
    }
    return transaction.Value().Commit();
}

base::Status SqliteStore::DeleteQueue(model::QueueId id)
{
    base::Result<Transaction> transaction = Transaction::Begin(connection_);
    if (!transaction) {
        return transaction.GetError();
    }
    base::Result<StoredQueue> stored = FindQueue(connection_, id);
    if (!stored) {
        return stored.GetError();
    }
    if (stored.Value().kind != model::QueueKind::Custom) {
        return base::MakeError(base::ErrorCode::InvalidArgument, "a built-in queue cannot be deleted");
    }
    // The members' queue_id is cleared by the foreign key.
    base::Result<Statement> remove = connection_.Prepare("DELETE FROM queues WHERE id = ?1");
    if (!remove) {
        return remove.GetError();
    }
    base::Status removed = remove.Value().Bind(1, id.value).Run();
    if (!removed) {
        return removed;
    }
    if (stored.Value().scheduleId) {
        base::Result<Statement> schedule = connection_.Prepare("DELETE FROM schedules WHERE id = ?1");
        if (!schedule) {
            return schedule.GetError();
        }
        base::Status scheduleRemoved = schedule.Value().Bind(1, *stored.Value().scheduleId).Run();
        if (!scheduleRemoved) {
            return scheduleRemoved;
        }
    }
    return transaction.Value().Commit();
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

// ---- Site Grabber ----

namespace {

constexpr std::string_view kSelectGrabberProjects = R"sql(
SELECT p.id, p.name, p.is_template, p.definition, p.login_user, p.login_secret, p.schedule_mode, p.created_at,
       p.last_run_at, s.id, s.enabled, s.mode, s.start_minutes, s.start_day, s.weekdays, s.repeat_minutes,
       s.stop_enabled, s.stop_minutes, s.start_on_app_start, s.last_started_at
FROM grabber_projects p LEFT JOIN schedules s ON s.id = p.schedule_id
ORDER BY p.name, p.id
)sql";

// Binds ?2..?9 of the project columns that can change; the caller binds ?1.
void BindGrabberProjectColumns(Statement& statement, const model::GrabberProject& project, std::string_view definition,
                               std::int64_t scheduleId)
{
    statement.Bind(2, project.name).Bind(3, project.isTemplate).Bind(4, definition).Bind(5, project.loginUser);
    if (project.loginSecret.empty()) {
        statement.BindNull(6);
    } else {
        statement.Bind(6, std::span<const std::uint8_t>(project.loginSecret));
    }
    statement.Bind(7, scheduleId).Bind(8, model::ToString(project.scheduleMode)).Bind(9, project.lastRunAtMs);
}

std::optional<std::int64_t> FindGrabberSchedule(Connection& connection, model::GrabberProjectId id,
                                                base::Status& status)
{
    base::Result<Statement> query = connection.Prepare("SELECT schedule_id FROM grabber_projects WHERE id = ?1");
    if (!query) {
        status = query.GetError();
        return std::nullopt;
    }
    query.Value().Bind(1, id.value);
    base::Result<bool> row = query.Value().Step();
    if (!row) {
        status = row.GetError();
        return std::nullopt;
    }
    if (!row.Value()) {
        status = base::MakeError(base::ErrorCode::NotFound, "Grabber project does not exist");
        return std::nullopt;
    }
    return query.Value().OptionalInt64(0);
}

}  // namespace

base::Result<std::vector<core::StoredGrabberProject>> SqliteStore::LoadGrabberProjects()
{
    base::Result<Statement> query = connection_.Prepare(kSelectGrabberProjects);
    if (!query) {
        return query.GetError();
    }
    std::vector<core::StoredGrabberProject> projects;
    for (;;) {
        base::Result<bool> row = query.Value().Step();
        if (!row) {
            return row.GetError();
        }
        if (!row.Value()) {
            return projects;
        }
        const Statement& r = query.Value();
        core::StoredGrabberProject stored;
        model::GrabberProject& project = stored.project;
        project.id = model::GrabberProjectId{r.Int64(0)};
        project.name = r.Text(1);
        project.isTemplate = r.Int64(2) != 0;
        stored.definitionJson = r.Text(3);
        project.loginUser = r.Text(4);
        project.loginSecret = r.Blob(5);
        const auto mode = model::ParseGrabberScheduleMode(r.Text(6));
        if (!mode) {
            return CorruptRow("grabber_projects.schedule_mode");
        }
        project.scheduleMode = *mode;
        project.createdAtMs = r.Int64(7);
        project.lastRunAtMs = r.OptionalInt64(8);
        base::Result<model::Schedule> schedule = ReadSchedule(r, 9);
        if (!schedule) {
            return schedule.GetError();
        }
        project.schedule = std::move(schedule).Value();
        projects.push_back(std::move(stored));
    }
}

base::Result<model::GrabberProjectId> SqliteStore::InsertGrabberProject(const model::GrabberProject& project,
                                                                        std::string_view definitionJson)
{
    base::Result<Transaction> transaction = Transaction::Begin(connection_);
    if (!transaction) {
        return transaction.GetError();
    }
    base::Result<std::int64_t> schedule = WriteSchedule(std::nullopt, project.schedule);
    if (!schedule) {
        return schedule.GetError();
    }
    base::Result<Statement> insert = connection_.Prepare(
        "INSERT INTO grabber_projects (created_at, name, is_template, definition, login_user, login_secret, "
        "schedule_id, schedule_mode, last_run_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)");
    if (!insert) {
        return insert.GetError();
    }
    insert.Value().Bind(1, project.createdAtMs);
    BindGrabberProjectColumns(insert.Value(), project, definitionJson, schedule.Value());
    base::Status inserted = insert.Value().Run();
    if (!inserted) {
        return inserted.GetError();
    }
    const model::GrabberProjectId id{connection_.LastInsertRowId()};
    base::Status committed = transaction.Value().Commit();
    if (!committed) {
        return committed.GetError();
    }
    return id;
}

base::Status SqliteStore::SaveGrabberProject(const model::GrabberProject& project, std::string_view definitionJson)
{
    base::Result<Transaction> transaction = Transaction::Begin(connection_);
    if (!transaction) {
        return transaction.GetError();
    }
    base::Status found;
    const std::optional<std::int64_t> scheduleId = FindGrabberSchedule(connection_, project.id, found);
    if (!found) {
        return found;
    }
    base::Result<std::int64_t> schedule = WriteSchedule(scheduleId, project.schedule);
    if (!schedule) {
        return schedule.GetError();
    }
    base::Result<Statement> update = connection_.Prepare(
        "UPDATE grabber_projects SET name = ?2, is_template = ?3, definition = ?4, login_user = ?5, "
        "login_secret = ?6, schedule_id = ?7, schedule_mode = ?8, last_run_at = ?9 WHERE id = ?1");
    if (!update) {
        return update.GetError();
    }
    update.Value().Bind(1, project.id.value);
    BindGrabberProjectColumns(update.Value(), project, definitionJson, schedule.Value());
    base::Status updated = update.Value().Run();
    if (!updated) {
        return updated;
    }
    return transaction.Value().Commit();
}

base::Status SqliteStore::DeleteGrabberProject(model::GrabberProjectId id)
{
    base::Result<Transaction> transaction = Transaction::Begin(connection_);
    if (!transaction) {
        return transaction.GetError();
    }
    base::Status found;
    const std::optional<std::int64_t> scheduleId = FindGrabberSchedule(connection_, id, found);
    if (!found) {
        return found;
    }
    // Items go with the project (cascade); downloads lose their project (set null).
    base::Result<Statement> remove = connection_.Prepare("DELETE FROM grabber_projects WHERE id = ?1");
    if (!remove) {
        return remove.GetError();
    }
    base::Status removed = remove.Value().Bind(1, id.value).Run();
    if (!removed) {
        return removed;
    }
    if (scheduleId) {
        base::Result<Statement> schedule = connection_.Prepare("DELETE FROM schedules WHERE id = ?1");
        if (!schedule) {
            return schedule.GetError();
        }
        base::Status scheduleRemoved = schedule.Value().Bind(1, *scheduleId).Run();
        if (!scheduleRemoved) {
            return scheduleRemoved;
        }
    }
    return transaction.Value().Commit();
}

base::Result<std::vector<model::GrabberItem>> SqliteStore::LoadGrabberItems(model::GrabberProjectId id)
{
    base::Result<Statement> query = connection_.Prepare(
        "SELECT id, url, kind, parent_id, depth, link_text, size, content_type, checked, state, download_id, "
        "local_path FROM grabber_items WHERE project_id = ?1 ORDER BY id");
    if (!query) {
        return query.GetError();
    }
    query.Value().Bind(1, id.value);
    std::vector<model::GrabberItem> items;
    for (;;) {
        base::Result<bool> row = query.Value().Step();
        if (!row) {
            return row.GetError();
        }
        if (!row.Value()) {
            return items;
        }
        const Statement& r = query.Value();
        model::GrabberItem item;
        item.id = r.Int64(0);
        item.url = r.Text(1);
        const auto kind = model::ParseGrabberItemKind(r.Text(2));
        const auto state = model::ParseGrabberItemState(r.Text(9));
        const std::int64_t depth = r.Int64(4);
        if (!kind || !state || depth > std::numeric_limits<int>::max()) {
            return CorruptRow("grabber_items");
        }
        item.kind = *kind;
        item.parentId = r.OptionalInt64(3);
        item.depth = static_cast<int>(depth);
        item.linkText = r.Text(5);
        item.size = r.OptionalInt64(6);
        item.contentType = r.Text(7);
        item.checked = r.Int64(8) != 0;
        item.state = *state;
        if (const auto download = r.OptionalInt64(10)) {
            item.downloadId = model::DownloadId{*download};
        }
        item.localPath = r.Text(11);
        items.push_back(std::move(item));
    }
}

base::Status SqliteStore::SaveGrabberItems(model::GrabberProjectId id, std::span<const model::GrabberItem> items)
{
    base::Result<Transaction> transaction = Transaction::Begin(connection_);
    if (!transaction) {
        return transaction.GetError();
    }
    // A download deleted since the item was last saved is stored as none, not refused.
    base::Result<Statement> insert = connection_.Prepare(
        "INSERT OR REPLACE INTO grabber_items (project_id, id, url, kind, parent_id, depth, link_text, size, "
        "content_type, checked, state, download_id, local_path) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, "
        "?11, (SELECT id FROM downloads WHERE id = ?12), ?13)");
    if (!insert) {
        return insert.GetError();
    }
    for (const model::GrabberItem& item : items) {
        insert.Value().Reset();
        base::Status inserted =
            insert.Value()
                .Bind(1, id.value)
                .Bind(2, item.id)
                .Bind(3, item.url)
                .Bind(4, model::ToString(item.kind))
                .Bind(5, item.parentId)
                .Bind(6, item.depth)
                .Bind(7, item.linkText)
                .Bind(8, item.size)
                .Bind(9, item.contentType)
                .Bind(10, item.checked)
                .Bind(11, model::ToString(item.state))
                .Bind(12, item.downloadId ? std::optional<std::int64_t>(item.downloadId->value) : std::nullopt)
                .Bind(13, item.localPath)
                .Run();
        if (!inserted) {
            return inserted;
        }
    }
    return transaction.Value().Commit();
}

base::Status SqliteStore::ClearGrabberItems(model::GrabberProjectId id)
{
    base::Result<Statement> remove = connection_.Prepare("DELETE FROM grabber_items WHERE project_id = ?1");
    if (!remove) {
        return remove.GetError();
    }
    return remove.Value().Bind(1, id.value).Run();
}

}  // namespace ldm::storage
