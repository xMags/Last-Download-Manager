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
