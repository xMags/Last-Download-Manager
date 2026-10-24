#include "storage/Migrations.h"
#include "storage/SqliteStore.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using ldm::model::Category;
using ldm::model::CategoryId;
using ldm::model::Download;
using ldm::model::DownloadId;
using ldm::model::DownloadStatus;
using ldm::storage::SqliteStore;

SqliteStore OpenMemoryStore()
{
    auto store = SqliteStore::Open(":memory:");
    EXPECT_TRUE(store.Ok()) << (store.Ok() ? "" : store.GetError().message);
    return std::move(store).Value();
}

Download SampleDownload(std::string publicId)
{
    Download d;
    d.publicId = std::move(publicId);
    d.url = "https://example.test/file.bin";
    d.fileName = "file.bin";
    d.saveDir = "C:\\Downloads";
    d.status = DownloadStatus::Paused;
    d.totalSize = 1000;
    d.resumable = true;
    d.validators.etag = "\"abc\"";
    d.loginUser = "user";
    d.loginSecret = {1, 2, 3};
    d.cookieSecret = {4, 5, 6, 7};
    d.postSecret = {8, 9};
    d.postContentType = "application/x-www-form-urlencoded";
    d.createdAtMs = 1'700'000'000'000;
    d.segments = {{0, 500, 500}, {500, 1000, 120}};
    return d;
}

TEST(SqliteStore, InsertAndLoadRoundTrip)
{
    SqliteStore store = OpenMemoryStore();
    const Download original = SampleDownload("a");
    auto id = store.InsertDownload(original);
    ASSERT_TRUE(id.Ok()) << id.GetError().message;

    auto loaded = store.LoadDownloads();
    ASSERT_TRUE(loaded.Ok()) << loaded.GetError().message;
    ASSERT_EQ(loaded.Value().size(), 1u);
    const Download& d = loaded.Value().front();
    EXPECT_EQ(d.id, id.Value());
    EXPECT_EQ(d.publicId, "a");
    EXPECT_EQ(d.status, DownloadStatus::Paused);
    EXPECT_EQ(d.totalSize, 1000);
    EXPECT_EQ(d.resumable, true);
    EXPECT_EQ(d.validators.etag, "\"abc\"");
    EXPECT_FALSE(d.validators.lastModified.has_value());
    EXPECT_EQ(d.loginSecret, original.loginSecret);
    EXPECT_EQ(d.cookieSecret, original.cookieSecret);
    EXPECT_EQ(d.postSecret, original.postSecret);
    EXPECT_EQ(d.postContentType, original.postContentType);
    ASSERT_EQ(d.segments.size(), 2u);
    EXPECT_EQ(d.segments[1].start, 500);
    EXPECT_EQ(d.segments[1].end, 1000);
    EXPECT_EQ(d.segments[1].done, 120);
    EXPECT_EQ(d.DownloadedBytes(), 620);
}

TEST(SqliteStore, SaveReplacesSegments)
{
    SqliteStore store = OpenMemoryStore();
    Download d = SampleDownload("a");
    d.id = store.InsertDownload(d).Value();

    d.segments = {{0, std::nullopt, 42}};
    d.totalSize.reset();
    d.status = DownloadStatus::Downloading;
    ASSERT_TRUE(store.SaveDownload(d).Ok());

    const Download loaded = store.LoadDownloads().Value().front();
    EXPECT_EQ(loaded.status, DownloadStatus::Downloading);
    EXPECT_FALSE(loaded.totalSize.has_value());
    ASSERT_EQ(loaded.segments.size(), 1u);
    EXPECT_FALSE(loaded.segments[0].end.has_value());
    EXPECT_EQ(loaded.segments[0].done, 42);
}

TEST(SqliteStore, AwaitingConfirmationRoundTrips)
{
    SqliteStore store = OpenMemoryStore();
    Download d = SampleDownload("a");
    d.awaitingConfirmation = true;
    d.id = store.InsertDownload(d).Value();
    EXPECT_TRUE(store.LoadDownloads().Value().front().awaitingConfirmation);

    d.awaitingConfirmation = false;
    ASSERT_TRUE(store.SaveDownload(d).Ok());
    EXPECT_FALSE(store.LoadDownloads().Value().front().awaitingConfirmation);
}

TEST(SqliteStore, SavingMissingDownloadFailsWithoutSideEffects)
{
    SqliteStore store = OpenMemoryStore();
    Download ghost = SampleDownload("ghost");
    ghost.id = DownloadId{999};
    const auto status = store.SaveDownload(ghost);
    ASSERT_FALSE(status.Ok());
    EXPECT_EQ(status.GetError().code, ldm::base::ErrorCode::NotFound);
    EXPECT_TRUE(store.LoadDownloads().Value().empty());
}

TEST(SqliteStore, DuplicatePublicIdIsRejectedAtomically)
{
    SqliteStore store = OpenMemoryStore();
    ASSERT_TRUE(store.InsertDownload(SampleDownload("same")).Ok());
    EXPECT_FALSE(store.InsertDownload(SampleDownload("same")).Ok());
    const auto all = store.LoadDownloads().Value();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all.front().segments.size(), 2u);
}

TEST(SqliteStore, DeleteRemovesSegments)
{
    SqliteStore store = OpenMemoryStore();
    const DownloadId first = store.InsertDownload(SampleDownload("a")).Value();
    ASSERT_TRUE(store.InsertDownload(SampleDownload("b")).Ok());
    ASSERT_TRUE(store.DeleteDownload(first).Ok());
    const auto remaining = store.LoadDownloads().Value();
    ASSERT_EQ(remaining.size(), 1u);
    EXPECT_EQ(remaining.front().publicId, "b");
    EXPECT_EQ(remaining.front().segments.size(), 2u);
}

TEST(SqliteStore, DeletingCategoryClearsDownloadReference)
{
    SqliteStore store = OpenMemoryStore();
    Category video{CategoryId{}, "Video", "mp4 mkv", "", "C:\\Video", false, true, 1};
    const CategoryId categoryId = store.InsertCategory(video).Value();
    Download d = SampleDownload("a");
    d.categoryId = categoryId;
    ASSERT_TRUE(store.InsertDownload(d).Ok());

    ASSERT_TRUE(store.DeleteCategory(categoryId).Ok());
    EXPECT_FALSE(store.LoadDownloads().Value().front().categoryId.has_value());
    EXPECT_TRUE(store.LoadCategories().Value().empty());
}

TEST(SqliteStore, CategoryNamesAreUniqueIgnoringCase)
{
    SqliteStore store = OpenMemoryStore();
    ASSERT_TRUE(store.InsertCategory({CategoryId{}, "Music", "mp3", "", "C:\\M", false, true, 1}).Ok());
    EXPECT_FALSE(store.InsertCategory({CategoryId{}, "music", "", "", "C:\\M2", false, false, 2}).Ok());
}

TEST(SqliteStore, SettingsReplaceWholeMap)
{
    SqliteStore store = OpenMemoryStore();
    ASSERT_TRUE(store.SaveSettings({{"a", "1"}, {"b", "2"}}).Ok());
    ASSERT_TRUE(store.SaveSettings({{"b", "3"}}).Ok());
    const auto settings = store.LoadSettings().Value();
    ASSERT_EQ(settings.size(), 1u);
    EXPECT_EQ(settings.at("b"), "3");
}

TEST(SqliteStore, ConnectionRulesRoundTrip)
{
    SqliteStore store = OpenMemoryStore();
    ASSERT_TRUE(store.ReplaceServerLimits({{"*.example.test", 2}, {"cdn.test", 16}}).Ok());
    ASSERT_TRUE(store.ReplaceSiteLogins({{0, "files.test/private", "me", {9, 9}}}).Ok());
    const auto limits = store.LoadServerLimits().Value();
    ASSERT_EQ(limits.size(), 2u);
    EXPECT_EQ(limits[0].pattern, "*.example.test");
    EXPECT_EQ(limits[0].maxConnections, 2);
    const auto logins = store.LoadSiteLogins().Value();
    ASSERT_EQ(logins.size(), 1u);
    EXPECT_EQ(logins[0].secret, (std::vector<std::uint8_t>{9, 9}));
    EXPECT_FALSE(store.ReplaceServerLimits({{"bad", 99}}).Ok());
    EXPECT_EQ(store.LoadServerLimits().Value().size(), 2u);
}

Download SampleStream(std::string publicId)
{
    Download d = SampleDownload(std::move(publicId));
    d.kind = ldm::model::DownloadKind::Hls;
    d.mediaFromPrivateNetwork = true;
    d.segments.clear();
    d.totalSize.reset();

    ldm::model::MediaTrack video;
    video.role = ldm::model::TrackRole::Video;
    video.sourceUrl = "https://cdn.example.test/v/index.m3u8";
    video.codecs = "avc1.64001f,mp4a.40.2";
    video.keys.push_back({"https://cdn.example.test/key", std::nullopt});
    video.keys.push_back({"", ldm::model::AesKey{1, 2, 3}});
    ldm::model::MediaPiece init;
    init.kind = ldm::model::MediaPiece::Kind::Init;
    init.url = "https://cdn.example.test/v/init.mp4";
    init.rangeOffset = 0;
    init.rangeLength = 700;
    video.pieces.push_back(init);
    for (std::uint32_t i = 0; i < 3; ++i) {
        ldm::model::MediaPiece piece;
        piece.url = "https://cdn.example.test/v/" + std::to_string(i) + ".m4s";
        piece.durationUs = 4'000'000;
        piece.sequence = 0xFFFF'FFFF'FFFF'FFF0ull + i;  // above INT64_MAX: stored as two's complement
        piece.discontinuity = i == 2;
        piece.init = 0;
        piece.key = i % 2;
        piece.iv = ldm::model::AesKey{static_cast<std::uint8_t>(i)};
        video.pieces.push_back(piece);
    }
    video.pieces[1].done = true;
    video.pieces[1].bytes = 1234;
    d.tracks.push_back(video);

    ldm::model::MediaTrack audio;
    audio.role = ldm::model::TrackRole::Audio;
    audio.sourceUrl = "https://cdn.example.test/a.mpd";
    audio.language = "en";
    audio.name = "English";
    audio.wholeFile = true;
    ldm::model::MediaPiece whole;
    whole.url = "https://cdn.example.test/a.mp4";
    audio.pieces.push_back(whole);
    audio.ranges = {{0, 5000, 5000}, {5000, 9000, 10}};
    d.tracks.push_back(audio);
    return d;
}

TEST(SqliteStore, StreamRoundTripsWithTracksKeysPiecesAndRanges)
{
    SqliteStore store = OpenMemoryStore();
    const Download original = SampleStream("s");
    auto id = store.InsertDownload(original);
    ASSERT_TRUE(id.Ok()) << id.GetError().message;

    auto loaded = store.LoadDownloads();
    ASSERT_TRUE(loaded.Ok()) << loaded.GetError().message;
    const Download& d = loaded.Value().front();
    EXPECT_EQ(d.kind, ldm::model::DownloadKind::Hls);
    EXPECT_TRUE(d.mediaFromPrivateNetwork);
    EXPECT_TRUE(d.segments.empty());
    ASSERT_EQ(d.tracks.size(), 2u);
    const auto& video = d.tracks[0];
    EXPECT_EQ(video.codecs, "avc1.64001f,mp4a.40.2");
    ASSERT_EQ(video.keys.size(), 2u);
    EXPECT_EQ(video.keys[0].url, "https://cdn.example.test/key");
    EXPECT_FALSE(video.keys[0].value.has_value());
    EXPECT_EQ(video.keys[1].value, (ldm::model::AesKey{1, 2, 3}));
    ASSERT_EQ(video.pieces.size(), 4u);
    EXPECT_EQ(video.pieces[0].kind, ldm::model::MediaPiece::Kind::Init);
    EXPECT_EQ(video.pieces[0].rangeLength, 700);
    EXPECT_EQ(video.pieces[2].sequence, 0xFFFF'FFFF'FFFF'FFF1ull);
    EXPECT_EQ(video.pieces[2].init, 0u);
    EXPECT_EQ(video.pieces[2].key, 1u);
    EXPECT_EQ(video.pieces[2].iv, (ldm::model::AesKey{1}));
    EXPECT_EQ(video.pieces[1].done, true);
    EXPECT_EQ(video.pieces[1].bytes, 1234);
    EXPECT_TRUE(video.pieces[3].discontinuity);
    EXPECT_EQ(video.pieces[3].durationUs, 4'000'000);
    const auto& audio = d.tracks[1];
    EXPECT_EQ(audio.role, ldm::model::TrackRole::Audio);
    EXPECT_EQ(audio.language, "en");
    EXPECT_TRUE(audio.wholeFile);
    ASSERT_EQ(audio.ranges.size(), 2u);
    EXPECT_EQ(audio.ranges[1].done, 10);
    EXPECT_EQ(d.DownloadedBytes(), 1234 + 5010);
}

TEST(SqliteStore, StreamProgressIsWrittenPieceByPiece)
{
    SqliteStore store = OpenMemoryStore();
    Download d = SampleStream("s");
    d.id = store.InsertDownload(d).Value();

    const std::vector<ldm::core::MediaPieceDone> done{{0, 2, 77}, {0, 3, 88}};
    ASSERT_TRUE(store.MarkMediaPiecesDone(d.id, done).Ok());
    // A checkpoint writes the row and the whole-file ranges, and leaves pieces alone.
    d.tracks[1].ranges[1].done = 4000;
    d.tracks[0].pieces[1].done = false;  // the store must not rewrite pieces from here
    ASSERT_TRUE(store.SaveDownload(d).Ok());

    const Download loaded = store.LoadDownloads().Value().front();
    EXPECT_TRUE(loaded.tracks[0].pieces[1].done);
    EXPECT_EQ(loaded.tracks[0].pieces[2].bytes, 77);
    EXPECT_EQ(loaded.tracks[0].pieces[3].bytes, 88);
    EXPECT_EQ(loaded.tracks[1].ranges[1].done, 4000);

    const std::vector<ldm::core::MediaPieceDone> missing{{0, 9, 1}};
    EXPECT_FALSE(store.MarkMediaPiecesDone(d.id, missing).Ok());
    EXPECT_FALSE(store.LoadDownloads().Value().front().tracks[0].pieces[0].done);  // rolled back
}

TEST(SqliteStore, ReplacingATrackRewritesItsPiecesAndKeys)
{
    SqliteStore store = OpenMemoryStore();
    Download d = SampleStream("s");
    d.id = store.InsertDownload(d).Value();

    ldm::model::MediaTrack listed = d.tracks[0];
    listed.keys.clear();
    listed.pieces.resize(1);
    listed.pieces[0].kind = ldm::model::MediaPiece::Kind::Media;
    listed.pieces[0].key.reset();
    listed.pieces[0].init.reset();
    ASSERT_TRUE(store.ReplaceMediaTrack(d.id, 0, listed).Ok());
    ldm::model::MediaTrack reset = d.tracks[1];
    reset.ranges.clear();
    ASSERT_TRUE(store.ReplaceMediaTrack(d.id, 1, reset).Ok());

    const Download loaded = store.LoadDownloads().Value().front();
    ASSERT_EQ(loaded.tracks.size(), 2u);
    EXPECT_TRUE(loaded.tracks[0].keys.empty());
    EXPECT_EQ(loaded.tracks[0].pieces.size(), 1u);
    EXPECT_TRUE(loaded.tracks[1].ranges.empty());
}

TEST(SqliteStore, ALiveTrackGrowsByAppendingKeysAndPieces)
{
    SqliteStore store = OpenMemoryStore();
    Download d = SampleStream("s");
    d.tracks[0].live = true;
    d.id = store.InsertDownload(d).Value();
    EXPECT_TRUE(store.LoadDownloads().Value().front().tracks[0].live);

    ldm::model::MediaTrack grown = d.tracks[0];
    grown.keys.push_back({"https://cdn.example.test/key2", std::nullopt});
    ldm::model::MediaPiece next;
    next.url = "https://cdn.example.test/v/3.m4s";
    next.durationUs = 2'000'000;
    next.sequence = 77;
    next.init = 0;
    next.key = 2;
    grown.pieces.push_back(next);
    grown.live = false;
    ASSERT_TRUE(store.AppendMediaTrack(d.id, 0, grown, 2, 4).Ok());

    const Download loaded = store.LoadDownloads().Value().front();
    const auto& track = loaded.tracks[0];
    EXPECT_FALSE(track.live);
    ASSERT_EQ(track.keys.size(), 3u);
    EXPECT_EQ(track.keys[2].url, "https://cdn.example.test/key2");
    ASSERT_EQ(track.pieces.size(), 5u);
    EXPECT_EQ(track.pieces[4].sequence, 77u);
    EXPECT_EQ(track.pieces[4].key, 2u);
    EXPECT_TRUE(track.pieces[1].done) << "earlier pieces keep their progress";
    EXPECT_EQ(track.pieces[1].bytes, 1234);

    // Nothing new only changes the flag.
    grown.live = true;
    ASSERT_TRUE(store.AppendMediaTrack(d.id, 0, grown, 3, 5).Ok());
    EXPECT_TRUE(store.LoadDownloads().Value().front().tracks[0].live);
    EXPECT_EQ(store.LoadDownloads().Value().front().tracks[0].pieces.size(), 5u);
}

TEST(SqliteStore, SkippedPiecesRoundTrip)
{
    SqliteStore store = OpenMemoryStore();
    Download d = SampleStream("s");
    d.tracks[0].pieces[3].skipped = true;
    d.id = store.InsertDownload(d).Value();
    Download loaded = store.LoadDownloads().Value().front();
    EXPECT_TRUE(loaded.tracks[0].pieces[3].skipped);
    EXPECT_FALSE(loaded.tracks[0].pieces[2].skipped);

    loaded.tracks[0].pieces[2].skipped = true;
    ASSERT_TRUE(store.ReplaceMediaTrack(d.id, 0, loaded.tracks[0]).Ok());
    EXPECT_TRUE(store.LoadDownloads().Value().front().tracks[0].pieces[2].skipped);
}

TEST(SqliteStore, AppendingToATrackThatDoesNotExistChangesNothing)
{
    SqliteStore store = OpenMemoryStore();
    Download d = SampleStream("s");
    d.id = store.InsertDownload(d).Value();
    EXPECT_FALSE(store.AppendMediaTrack(d.id, 7, d.tracks[0], 2, 4).Ok());
    EXPECT_FALSE(store.AppendMediaTrack(d.id, 0, d.tracks[0], 3, 4).Ok()) << "starts past the end";
    // A piece index that is already stored fails the whole append.
    ldm::model::MediaTrack grown = d.tracks[0];
    grown.live = true;
    EXPECT_FALSE(store.AppendMediaTrack(d.id, 0, grown, 2, 3).Ok());
    const Download loaded = store.LoadDownloads().Value().front();
    EXPECT_FALSE(loaded.tracks[0].live) << "rolled back";
    EXPECT_EQ(loaded.tracks[0].pieces.size(), 4u);
}

TEST(SqliteStore, DeletingAStreamRemovesEverythingOfIt)
{
    SqliteStore store = OpenMemoryStore();
    const DownloadId kept = store.InsertDownload(SampleStream("keep")).Value();
    const DownloadId removed = store.InsertDownload(SampleStream("remove")).Value();
    ASSERT_TRUE(store.DeleteDownload(removed).Ok());
    const auto loaded = store.LoadDownloads().Value();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].id, kept);
    EXPECT_EQ(loaded[0].tracks.size(), 2u);
}

TEST(Migrations, SchemaFiveKeepsTheSegmentsOfEarlierDownloads)
{
    auto connection = ldm::storage::Connection::Open(":memory:");
    ASSERT_TRUE(connection.Ok());
    ASSERT_TRUE(connection.Value().Execute("PRAGMA foreign_keys = ON").Ok());
    ASSERT_TRUE(ldm::storage::MigrateTo(connection.Value(), 4).Ok());
    ASSERT_TRUE(connection.Value()
                    .Execute("INSERT INTO downloads (id, public_id, url, file_name, save_dir, status, created_at, "
                             "sort_order) VALUES (7, 'p', 'https://x.test/f', 'f', 'C:\\\\D', 'paused', 1, 1);"
                             "INSERT INTO segments (download_id, idx, start_offset, end_offset, done_bytes) "
                             "VALUES (7, 0, 0, 50, 50), (7, 1, 50, 100, 3);")
                    .Ok());
    const auto migrated = ldm::storage::Migrate(connection.Value());
    ASSERT_TRUE(migrated.Ok()) << migrated.GetError().message;

    auto query = connection.Value().Prepare(
        "SELECT d.kind, s.track, s.idx, s.done_bytes FROM downloads d JOIN segments s ON s.download_id = d.id "
        "ORDER BY s.idx");
    ASSERT_TRUE(query.Ok());
    ASSERT_TRUE(query.Value().Step().Value());
    EXPECT_EQ(query.Value().Text(0), "file");
    EXPECT_EQ(query.Value().Int64(1), 0);
    ASSERT_TRUE(query.Value().Step().Value());
    EXPECT_EQ(query.Value().Int64(2), 1);
    EXPECT_EQ(query.Value().Int64(3), 3);
    EXPECT_FALSE(query.Value().Step().Value());
    // The rebuilt table still goes with its download.
    ASSERT_TRUE(connection.Value().Execute("DELETE FROM downloads WHERE id = 7").Ok());
    auto count = connection.Value().Prepare("SELECT count(*) FROM segments");
    ASSERT_TRUE(count.Value().Step().Value());
    EXPECT_EQ(count.Value().Int64(0), 0);
}

TEST(Migrations, SchemaSixMarksEarlierTracksAsNotLive)
{
    auto connection = ldm::storage::Connection::Open(":memory:");
    ASSERT_TRUE(connection.Ok());
    ASSERT_TRUE(connection.Value().Execute("PRAGMA foreign_keys = ON").Ok());
    ASSERT_TRUE(ldm::storage::MigrateTo(connection.Value(), 5).Ok());
    ASSERT_TRUE(connection.Value()
                    .Execute("INSERT INTO downloads (id, public_id, url, file_name, save_dir, status, created_at, "
                             "sort_order, kind) VALUES (7, 'p', 'https://x.test/m.m3u8', 'f', 'C:\\\\D', 'paused', 1, "
                             "1, 'hls');"
                             "INSERT INTO media_tracks (download_id, track, role, source_url) "
                             "VALUES (7, 0, 'video', 'https://x.test/v.m3u8');")
                    .Ok());
    const auto migrated = ldm::storage::Migrate(connection.Value());
    ASSERT_TRUE(migrated.Ok()) << migrated.GetError().message;
    auto query = connection.Value().Prepare("SELECT live FROM media_tracks");
    ASSERT_TRUE(query.Ok());
    ASSERT_TRUE(query.Value().Step().Value());
    EXPECT_EQ(query.Value().Int64(0), 0);
    EXPECT_FALSE(connection.Value().Execute("UPDATE media_tracks SET live = 2").Ok()) << "only 0 or 1";
}

TEST(Migrations, SchemaSevenKeepsEarlierPiecesWanted)
{
    auto connection = ldm::storage::Connection::Open(":memory:");
    ASSERT_TRUE(connection.Ok());
    ASSERT_TRUE(connection.Value().Execute("PRAGMA foreign_keys = ON").Ok());
    ASSERT_TRUE(ldm::storage::MigrateTo(connection.Value(), 6).Ok());
    ASSERT_TRUE(connection.Value()
                    .Execute("INSERT INTO downloads (id, public_id, url, file_name, save_dir, status, created_at, "
                             "sort_order, kind) VALUES (7, 'p', 'https://x.test/m.m3u8', 'f', 'C:\\\\D', 'paused', 1, "
                             "1, 'hls');"
                             "INSERT INTO media_tracks (download_id, track, role, source_url) "
                             "VALUES (7, 0, 'video', 'https://x.test/v.m3u8');"
                             "INSERT INTO media_segments (download_id, track, idx, kind, url) "
                             "VALUES (7, 0, 0, 'media', 'https://x.test/0.ts');")
                    .Ok());
    ASSERT_TRUE(ldm::storage::Migrate(connection.Value()).Ok());
    auto query = connection.Value().Prepare("SELECT skipped FROM media_segments");
    ASSERT_TRUE(query.Ok());
    ASSERT_TRUE(query.Value().Step().Value());
    EXPECT_EQ(query.Value().Int64(0), 0);
}

TEST(Migrations, RefusesNewerSchema)
{
    auto connection = ldm::storage::Connection::Open(":memory:");
    ASSERT_TRUE(connection.Ok());
    ASSERT_TRUE(ldm::storage::Migrate(connection.Value()).Ok());
    ASSERT_TRUE(connection.Value().Execute("UPDATE schema_version SET version = 999").Ok());
    const auto status = ldm::storage::Migrate(connection.Value());
    ASSERT_FALSE(status.Ok());
    EXPECT_EQ(status.GetError().code, ldm::base::ErrorCode::Unsupported);
}

TEST(Migrations, IsIdempotent)
{
    auto connection = ldm::storage::Connection::Open(":memory:");
    ASSERT_TRUE(connection.Ok());
    ASSERT_TRUE(ldm::storage::Migrate(connection.Value()).Ok());
    ASSERT_TRUE(ldm::storage::Migrate(connection.Value()).Ok());
}

}  // namespace
