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
using ldm::model::Queue;
using ldm::model::QueueId;
using ldm::model::QueueKind;
using ldm::model::ScheduleMode;
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

Queue SampleQueue(std::string name)
{
    Queue queue;
    queue.name = std::move(name);
    queue.concurrent = 4;
    queue.retriesPerFile = 7;
    queue.completionAction = ldm::model::CompletionAction::Hibernate;
    queue.forceTerminate = true;
    queue.openFileWhenDone = "C:\\Tools\\done.cmd";
    queue.sortOrder = 5;
    queue.schedule.enabled = true;
    queue.schedule.mode = ScheduleMode::Once;
    queue.schedule.startMinutes = 23 * 60 + 30;
    queue.schedule.startDay = 20'000;
    queue.schedule.weekdays = 0b0010101;
    queue.schedule.repeatMinutes = 90;
    queue.schedule.stopEnabled = true;
    queue.schedule.stopMinutes = 6 * 60;
    queue.schedule.startOnAppStart = true;
    queue.schedule.lastStartedAtMs = 1'700'000'000'000;
    return queue;
}

TEST(SqliteStore, TheBuiltInQueuesExistFromTheStart)
{
    SqliteStore store = OpenMemoryStore();
    const auto queues = store.LoadQueues();
    ASSERT_TRUE(queues.Ok()) << queues.GetError().message;
    ASSERT_EQ(queues.Value().size(), 2u);
    EXPECT_EQ(queues.Value()[0].kind, QueueKind::Main);
    EXPECT_EQ(queues.Value()[0].name, "Main download queue");
    EXPECT_EQ(queues.Value()[1].kind, QueueKind::Sync);
    EXPECT_EQ(queues.Value()[1].name, "Synchronization queue");
    for (const Queue& queue : queues.Value()) {
        EXPECT_EQ(queue.concurrent, 1);
        EXPECT_EQ(queue.retriesPerFile, 3);
        EXPECT_EQ(queue.schedule, ldm::model::Schedule{});
    }
}

TEST(SqliteStore, QueueRoundTripsWithItsSchedule)
{
    SqliteStore store = OpenMemoryStore();
    const Queue original = SampleQueue("Nightly");
    const auto id = store.InsertQueue(original);
    ASSERT_TRUE(id.Ok()) << id.GetError().message;

    const auto queues = store.LoadQueues().Value();
    ASSERT_EQ(queues.size(), 3u);
    const Queue& loaded = queues.back();
    EXPECT_EQ(loaded.id, id.Value());
    EXPECT_EQ(loaded.name, "Nightly");
    EXPECT_EQ(loaded.kind, QueueKind::Custom);
    EXPECT_EQ(loaded.concurrent, 4);
    EXPECT_EQ(loaded.retriesPerFile, 7);
    EXPECT_EQ(loaded.completionAction, ldm::model::CompletionAction::Hibernate);
    EXPECT_TRUE(loaded.forceTerminate);
    EXPECT_EQ(loaded.openFileWhenDone, original.openFileWhenDone);
    EXPECT_EQ(loaded.sortOrder, 5);
    EXPECT_EQ(loaded.schedule, original.schedule);
}

TEST(SqliteStore, SavingAQueueChangesItAndItsSchedule)
{
    SqliteStore store = OpenMemoryStore();
    Queue queue = SampleQueue("Nightly");
    queue.id = store.InsertQueue(queue).Value();
    queue.name = "Weekend";
    queue.concurrent = 2;
    queue.schedule.mode = ScheduleMode::Daily;
    queue.schedule.startDay.reset();
    queue.schedule.lastStartedAtMs.reset();
    ASSERT_TRUE(store.SaveQueue(queue).Ok());

    const Queue loaded = store.LoadQueues().Value().back();
    EXPECT_EQ(loaded.name, "Weekend");
    EXPECT_EQ(loaded.concurrent, 2);
    EXPECT_EQ(loaded.schedule, queue.schedule);
}

TEST(SqliteStore, ABuiltInQueueGainsAScheduleWhenSaved)
{
    SqliteStore store = OpenMemoryStore();
    Queue main = store.LoadQueues().Value().front();
    main.schedule.enabled = true;
    main.schedule.startMinutes = 120;
    main.kind = QueueKind::Custom;  // ignored: a queue keeps its kind
    ASSERT_TRUE(store.SaveQueue(main).Ok());
    ASSERT_TRUE(store.SaveQueue(main).Ok());  // the second save updates the schedule it made

    const Queue loaded = store.LoadQueues().Value().front();
    EXPECT_EQ(loaded.kind, QueueKind::Main);
    EXPECT_TRUE(loaded.schedule.enabled);
    EXPECT_EQ(loaded.schedule.startMinutes, 120);
}

TEST(SqliteStore, SavingAQueueThatDoesNotExistFails)
{
    SqliteStore store = OpenMemoryStore();
    Queue queue = SampleQueue("Ghost");
    queue.id = QueueId{99};
    const auto status = store.SaveQueue(queue);
    ASSERT_FALSE(status.Ok());
    EXPECT_EQ(status.GetError().code, ldm::base::ErrorCode::NotFound);
}

TEST(SqliteStore, QueueNamesAreUniqueIgnoringCase)
{
    SqliteStore store = OpenMemoryStore();
    ASSERT_TRUE(store.InsertQueue(SampleQueue("Nightly")).Ok());
    EXPECT_FALSE(store.InsertQueue(SampleQueue("NIGHTLY")).Ok());
    EXPECT_FALSE(store.InsertQueue(SampleQueue("main download queue")).Ok());
    EXPECT_EQ(store.LoadQueues().Value().size(), 3u);
}

TEST(SqliteStore, OnlyCustomQueuesCanBeAdded)
{
    SqliteStore store = OpenMemoryStore();
    Queue queue = SampleQueue("Second main");
    queue.kind = QueueKind::Main;
    const auto id = store.InsertQueue(queue);
    ASSERT_FALSE(id.Ok());
    EXPECT_EQ(id.GetError().code, ldm::base::ErrorCode::InvalidArgument);
}

TEST(SqliteStore, AQueueOutOfRangeIsRefused)
{
    SqliteStore store = OpenMemoryStore();
    Queue queue = SampleQueue("Wide");
    queue.concurrent = ldm::model::kMaxQueueConcurrent + 1;
    EXPECT_FALSE(store.InsertQueue(queue).Ok());
    queue.concurrent = 1;
    queue.schedule.startMinutes = ldm::model::kMinutesPerDay;
    EXPECT_FALSE(store.InsertQueue(queue).Ok());
    EXPECT_EQ(store.LoadQueues().Value().size(), 2u);
}

TEST(SqliteStore, DeletingAQueueKeepsItsMembersOutsideAnyQueue)
{
    SqliteStore store = OpenMemoryStore();
    const QueueId queue = store.InsertQueue(SampleQueue("Nightly")).Value();
    Download d = SampleDownload("a");
    d.queueId = queue;
    d.queuePosition = 4;
    d.queueFailures = 2;
    ASSERT_TRUE(store.InsertDownload(d).Ok());

    ASSERT_TRUE(store.DeleteQueue(queue).Ok());
    EXPECT_FALSE(store.LoadDownloads().Value().front().queueId.has_value());
    EXPECT_EQ(store.LoadQueues().Value().size(), 2u);
    const auto again = store.DeleteQueue(queue);
    ASSERT_FALSE(again.Ok());
    EXPECT_EQ(again.GetError().code, ldm::base::ErrorCode::NotFound);
}

TEST(SqliteStore, TheBuiltInQueuesCannotBeDeleted)
{
    SqliteStore store = OpenMemoryStore();
    for (const Queue& queue : store.LoadQueues().Value()) {
        const auto status = store.DeleteQueue(queue.id);
        ASSERT_FALSE(status.Ok());
        EXPECT_EQ(status.GetError().code, ldm::base::ErrorCode::InvalidArgument);
    }
    EXPECT_EQ(store.LoadQueues().Value().size(), 2u);
}

ldm::model::GrabberProject SampleProject(std::string name)
{
    ldm::model::GrabberProject project;
    project.name = std::move(name);
    project.loginUser = "me";
    project.loginSecret = {1, 2, 3};
    project.scheduleMode = ldm::model::GrabberScheduleMode::ExploreDownload;
    project.schedule.enabled = true;
    project.schedule.mode = ScheduleMode::Once;
    project.schedule.startMinutes = 600;
    project.schedule.startDay = 20'000;
    project.createdAtMs = 1'700'000'000'000;
    return project;
}

TEST(SqliteStore, GrabberProjectRoundTripsWithItsSchedule)
{
    SqliteStore store = OpenMemoryStore();
    const auto project = SampleProject("Gallery");
    auto id = store.InsertGrabberProject(project, R"({"version":1})");
    ASSERT_TRUE(id.Ok()) << id.GetError().message;
    auto loaded = store.LoadGrabberProjects();
    ASSERT_TRUE(loaded.Ok()) << loaded.GetError().message;
    ASSERT_EQ(loaded.Value().size(), 1u);
    auto expected = project;
    expected.id = id.Value();
    EXPECT_EQ(loaded.Value()[0].project, expected);
    EXPECT_EQ(loaded.Value()[0].definitionJson, R"({"version":1})");

    expected.name = "Renamed";
    expected.loginSecret.clear();
    expected.lastRunAtMs = 5;
    expected.schedule.enabled = false;
    expected.isTemplate = true;
    ASSERT_TRUE(store.SaveGrabberProject(expected, "{}").Ok());
    loaded = store.LoadGrabberProjects();
    ASSERT_TRUE(loaded.Ok());
    EXPECT_EQ(loaded.Value()[0].project, expected);
    EXPECT_EQ(loaded.Value()[0].definitionJson, "{}");

    EXPECT_FALSE(store.InsertGrabberProject(SampleProject("renamed"), "{}").Ok()) << "names are unique ignoring case";
    auto missing = expected;
    missing.id = ldm::model::GrabberProjectId{99};
    EXPECT_FALSE(store.SaveGrabberProject(missing, "{}").Ok());
}

TEST(SqliteStore, GrabberItemsRoundTripAndGoWithTheirProject)
{
    SqliteStore store = OpenMemoryStore();
    auto id = store.InsertGrabberProject(SampleProject("Site"), "{}");
    ASSERT_TRUE(id.Ok());
    auto download = SampleDownload("g");
    download.projectId = id.Value();
    download.listed = false;
    auto downloadId = store.InsertDownload(download);
    ASSERT_TRUE(downloadId.Ok()) << downloadId.GetError().message;

    std::vector<ldm::model::GrabberItem> items(2);
    items[0].id = 1;
    items[0].url = "https://example.test/";
    items[0].kind = ldm::model::GrabberItemKind::Page;
    items[0].state = ldm::model::GrabberItemState::Done;
    items[0].localPath = "C:\\Mirror\\index.html";
    items[1].id = 2;
    items[1].url = "https://example.test/a.zip";
    items[1].parentId = 1;
    items[1].depth = 1;
    items[1].linkText = "A";
    items[1].size = 1234;
    items[1].contentType = "application/zip";
    items[1].checked = true;
    items[1].state = ldm::model::GrabberItemState::Downloading;
    items[1].downloadId = downloadId.Value();
    ASSERT_TRUE(store.SaveGrabberItems(id.Value(), items).Ok());
    auto loaded = store.LoadGrabberItems(id.Value());
    ASSERT_TRUE(loaded.Ok()) << loaded.GetError().message;
    EXPECT_EQ(loaded.Value(), items);

    // Saving again replaces; a download deleted in between is stored as none.
    items[1].state = ldm::model::GrabberItemState::Done;
    ASSERT_TRUE(store.DeleteDownload(downloadId.Value()).Ok());
    ASSERT_TRUE(store.SaveGrabberItems(id.Value(), std::span(items).subspan(1)).Ok());
    loaded = store.LoadGrabberItems(id.Value());
    ASSERT_TRUE(loaded.Ok());
    ASSERT_EQ(loaded.Value().size(), 2u);
    EXPECT_EQ(loaded.Value()[1].state, ldm::model::GrabberItemState::Done);
    EXPECT_FALSE(loaded.Value()[1].downloadId);

    // The downloads of a deleted project stay, without it.
    auto kept = SampleDownload("k");
    kept.projectId = id.Value();
    ASSERT_TRUE(store.InsertDownload(kept).Ok());
    ASSERT_TRUE(store.ClearGrabberItems(id.Value()).Ok());
    EXPECT_TRUE(store.LoadGrabberItems(id.Value()).Value().empty());
    ASSERT_TRUE(store.SaveGrabberItems(id.Value(), items).Ok());
    ASSERT_TRUE(store.DeleteGrabberProject(id.Value()).Ok());
    EXPECT_TRUE(store.LoadGrabberProjects().Value().empty());
    EXPECT_TRUE(store.LoadGrabberItems(id.Value()).Value().empty());
    const auto downloads = store.LoadDownloads();
    ASSERT_TRUE(downloads.Ok());
    ASSERT_EQ(downloads.Value().size(), 1u);
    EXPECT_FALSE(downloads.Value()[0].projectId);
    EXPECT_FALSE(store.DeleteGrabberProject(id.Value()).Ok());
}

TEST(SqliteStore, ProjectAndListedRoundTripOnDownloads)
{
    SqliteStore store = OpenMemoryStore();
    auto project = store.InsertGrabberProject(SampleProject("Site"), "{}");
    ASSERT_TRUE(project.Ok());
    auto download = SampleDownload("h");
    download.projectId = project.Value();
    download.listed = false;
    auto id = store.InsertDownload(download);
    ASSERT_TRUE(id.Ok());
    auto loaded = store.LoadDownloads();
    ASSERT_TRUE(loaded.Ok());
    EXPECT_EQ(loaded.Value()[0].projectId, project.Value());
    EXPECT_FALSE(loaded.Value()[0].listed);
    download.id = id.Value();
    download.listed = true;
    ASSERT_TRUE(store.SaveDownload(download).Ok());
    EXPECT_TRUE(store.LoadDownloads().Value()[0].listed);
}

TEST(SqliteStore, ReplaceFinalFileRoundTrips)
{
    SqliteStore store = OpenMemoryStore();
    auto download = SampleDownload("r");
    download.replaceFinalFile = true;
    auto id = store.InsertDownload(download);
    ASSERT_TRUE(id.Ok());
    EXPECT_TRUE(store.LoadDownloads().Value()[0].replaceFinalFile);
    download.id = id.Value();
    download.replaceFinalFile = false;
    ASSERT_TRUE(store.SaveDownload(download).Ok());
    EXPECT_FALSE(store.LoadDownloads().Value()[0].replaceFinalFile);
}

TEST(SqliteStore, QueueMembershipRoundTrips)
{
    SqliteStore store = OpenMemoryStore();
    const QueueId main = store.LoadQueues().Value().front().id;
    Download d = SampleDownload("a");
    d.queueId = main;
    d.queuePosition = 12;
    d.queueFailures = 1;
    ASSERT_TRUE(store.InsertDownload(d).Ok());
    Download loaded = store.LoadDownloads().Value().front();
    EXPECT_EQ(loaded.queueId, main);
    EXPECT_EQ(loaded.queuePosition, 12);
    EXPECT_EQ(loaded.queueFailures, 1);

    loaded.queueId.reset();
    loaded.queuePosition = 0;
    loaded.queueFailures = 0;
    ASSERT_TRUE(store.SaveDownload(loaded).Ok());
    loaded = store.LoadDownloads().Value().front();
    EXPECT_FALSE(loaded.queueId.has_value());
    EXPECT_EQ(loaded.queueFailures, 0);
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

TEST(SqliteStore, SiteVideoKeepsItsFlagAndTrackFormatsAcrossUpdates)
{
    SqliteStore store = OpenMemoryStore();
    Download original = SampleStream("site");
    original.kind = ldm::model::DownloadKind::Dash;
    original.siteVideo = true;
    original.url = "https://www.youtube.com/watch?v=aqz-KE-bpKQ";
    original.tracks[1].siteFormat = "140";
    original.tracks[1].expectedBytes = 10'271'496;
    auto id = store.InsertDownload(original);
    ASSERT_TRUE(id.Ok()) << id.GetError().message;

    auto loaded = store.LoadDownloads();
    ASSERT_TRUE(loaded.Ok()) << loaded.GetError().message;
    Download d = loaded.Value().front();
    EXPECT_TRUE(d.siteVideo);
    EXPECT_EQ(d.tracks[0].siteFormat, "");
    EXPECT_FALSE(d.tracks[0].expectedBytes);
    EXPECT_EQ(d.tracks[1].siteFormat, "140");
    EXPECT_EQ(d.tracks[1].expectedBytes, 10'271'496);

    d.siteVideo = false;
    ASSERT_TRUE(store.SaveDownload(d).Ok());
    EXPECT_FALSE(store.LoadDownloads().Value().front().siteVideo);
    // Every other download is untouched by the new columns.
    ASSERT_TRUE(store.InsertDownload(SampleDownload("plain")).Ok());
    for (const Download& other : store.LoadDownloads().Value()) {
        EXPECT_FALSE(other.siteVideo) << other.publicId;
    }
}

TEST(SqliteStore, ASiteVideoFromAListKeepsItsQualityRule)
{
    SqliteStore store = OpenMemoryStore();
    Download original = SampleDownload("listed");
    original.kind = ldm::model::DownloadKind::Dash;
    original.siteVideo = true;
    original.siteQuality = ldm::model::SiteQuality::UpTo720;
    original.url = "https://www.youtube.com/watch?v=GvgqDSnpRQM";
    ASSERT_TRUE(store.InsertDownload(original).Ok());
    Download d = store.LoadDownloads().Value().front();
    EXPECT_EQ(d.siteQuality, ldm::model::SiteQuality::UpTo720);
    EXPECT_TRUE(d.tracks.empty());

    d.siteQuality = ldm::model::SiteQuality::AudioOnly;
    ASSERT_TRUE(store.SaveDownload(d).Ok());
    EXPECT_EQ(store.LoadDownloads().Value().front().siteQuality, ldm::model::SiteQuality::AudioOnly);
    d.siteQuality.reset();
    ASSERT_TRUE(store.SaveDownload(d).Ok());
    EXPECT_FALSE(store.LoadDownloads().Value().front().siteQuality);
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

TEST(Migrations, SchemaEightLeavesEarlierDownloadsOutsideAnyQueue)
{
    auto connection = ldm::storage::Connection::Open(":memory:");
    ASSERT_TRUE(connection.Ok());
    ASSERT_TRUE(connection.Value().Execute("PRAGMA foreign_keys = ON").Ok());
    ASSERT_TRUE(ldm::storage::MigrateTo(connection.Value(), 7).Ok());
    ASSERT_TRUE(connection.Value()
                    .Execute("INSERT INTO downloads (id, public_id, url, file_name, save_dir, status, created_at, "
                             "sort_order) VALUES (7, 'p', 'https://x.test/f', 'f', 'C:\\\\D', 'paused', 1, 1);")
                    .Ok());
    const auto migrated = ldm::storage::Migrate(connection.Value());
    ASSERT_TRUE(migrated.Ok()) << migrated.GetError().message;
    auto query = connection.Value().Prepare("SELECT queue_id, queue_position, queue_failures FROM downloads");
    ASSERT_TRUE(query.Ok());
    ASSERT_TRUE(query.Value().Step().Value());
    EXPECT_FALSE(query.Value().OptionalInt64(0).has_value());
    EXPECT_EQ(query.Value().Int64(1), 0);
    EXPECT_EQ(query.Value().Int64(2), 0);
    // A second main queue is refused by the schema itself.
    EXPECT_FALSE(
        connection.Value().Execute("INSERT INTO queues (name, kind, sort_order) VALUES ('Another', 'main', 9)").Ok());
}

TEST(Migrations, SchemaNineListsEarlierDownloads)
{
    auto connection = ldm::storage::Connection::Open(":memory:");
    ASSERT_TRUE(connection.Ok());
    ASSERT_TRUE(connection.Value().Execute("PRAGMA foreign_keys = ON").Ok());
    ASSERT_TRUE(ldm::storage::MigrateTo(connection.Value(), 8).Ok());
    ASSERT_TRUE(connection.Value()
                    .Execute("INSERT INTO downloads (id, public_id, url, file_name, save_dir, status, created_at, "
                             "sort_order) VALUES (7, 'p', 'https://x.test/f', 'f', 'C:\\\\D', 'paused', 1, 1);")
                    .Ok());
    const auto migrated = ldm::storage::Migrate(connection.Value());
    ASSERT_TRUE(migrated.Ok()) << migrated.GetError().message;
    auto query = connection.Value().Prepare("SELECT project_id, listed FROM downloads");
    ASSERT_TRUE(query.Ok());
    ASSERT_TRUE(query.Value().Step().Value());
    EXPECT_FALSE(query.Value().OptionalInt64(0).has_value());
    EXPECT_EQ(query.Value().Int64(1), 1);
}

TEST(Migrations, SchemaTenReplacesNoEarlierFile)
{
    auto connection = ldm::storage::Connection::Open(":memory:");
    ASSERT_TRUE(connection.Ok());
    ASSERT_TRUE(connection.Value().Execute("PRAGMA foreign_keys = ON").Ok());
    ASSERT_TRUE(ldm::storage::MigrateTo(connection.Value(), 9).Ok());
    ASSERT_TRUE(connection.Value()
                    .Execute("INSERT INTO downloads (id, public_id, url, file_name, save_dir, status, created_at, "
                             "sort_order) VALUES (7, 'p', 'https://x.test/f', 'f', 'C:\\\\D', 'paused', 1, 1);")
                    .Ok());
    const auto migrated = ldm::storage::Migrate(connection.Value());
    ASSERT_TRUE(migrated.Ok()) << migrated.GetError().message;
    auto query = connection.Value().Prepare("SELECT replace_final FROM downloads");
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
