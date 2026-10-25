#include "base/Paths.h"
#include "base/Result.h"
#include "base/Utf.h"
#include "core/Coordinator.h"
#include "core/MediaDownloads.h"
#include "storage/SqliteStore.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace {

using ldm::core::AddDownloadCommand;
using ldm::core::AddMode;
using ldm::core::AddResult;
using ldm::core::Coordinator;
using ldm::core::EngineEvent;
using ldm::core::FailureClass;
using ldm::core::FailureReason;
using ldm::core::ProgressInfo;
using ldm::core::ResponseInfo;
using ldm::core::TransferFailure;
using ldm::core::TransferFinished;
using ldm::core::TransferOutcome;
using ldm::model::DownloadId;
using ldm::model::DownloadStatus;
using ldm::model::Segment;
namespace fs = std::filesystem;

const fs::path kRoot = LR"(C:\Users\Test\Downloads)";

class FakeEngine final : public ldm::core::TransferEngine {
public:
    void Start(ldm::core::TransferRequest request) override { starts.push_back(std::move(request)); }
    void StartMedia(ldm::core::MediaRequest request) override { mediaStarts.push_back(std::move(request)); }
    void ExtendMedia(DownloadId, std::int64_t epoch, ldm::core::MediaExtension extension) override
    {
        extensions.emplace_back(epoch, std::move(extension));
    }
    void Stop(DownloadId id, std::int64_t epoch) override { stops.emplace_back(id, epoch); }
    void SetSpeedLimit(DownloadId, std::int64_t, std::optional<std::int64_t> limit) override { downloadLimit = limit; }
    void SetGlobalSpeedLimit(std::optional<std::int64_t> limit) override { globalLimit = limit; }
    void Finalize(ldm::core::FinalizeRequest request) override { finalizes.push_back(std::move(request)); }
    void Fetch(ldm::core::FetchRequest request) override { fetches.push_back(std::move(request)); }

    std::vector<ldm::core::TransferRequest> starts;
    std::vector<ldm::core::MediaRequest> mediaStarts;
    std::vector<std::pair<std::int64_t, ldm::core::MediaExtension>> extensions;
    std::vector<std::pair<DownloadId, std::int64_t>> stops;
    std::vector<ldm::core::FinalizeRequest> finalizes;
    std::vector<ldm::core::FetchRequest> fetches;
    std::optional<std::int64_t> downloadLimit;
    std::optional<std::int64_t> globalLimit;
};

class FakeMediaTools final : public ldm::core::MediaTools {
public:
    void Mux(ldm::core::MuxRequest request) override { muxes.push_back(std::move(request)); }
    void Describe(ldm::core::DescribeRequest request) override { describes.push_back(std::move(request)); }
    void Cancel(DownloadId id, std::int64_t epoch) override { cancels.emplace_back(id, epoch); }

    std::vector<ldm::core::MuxRequest> muxes;
    std::vector<ldm::core::DescribeRequest> describes;
    std::vector<std::pair<DownloadId, std::int64_t>> cancels;
};

class FakeFileSystem final : public ldm::core::FileSystem {
public:
    bool Exists(const fs::path& path) override { return files.contains(ldm::base::PathComparisonKey(path)); }
    std::optional<std::int64_t> FileSize(const fs::path& path) override
    {
        const auto it = files.find(ldm::base::PathComparisonKey(path));
        return it == files.end() ? std::nullopt : std::optional<std::int64_t>(it->second);
    }
    ldm::base::Status Remove(const fs::path& path) override
    {
        files.erase(ldm::base::PathComparisonKey(path));
        removed.push_back(path);
        return {};
    }
    ldm::base::Status Recycle(const fs::path& path) override
    {
        files.erase(ldm::base::PathComparisonKey(path));
        recycled.push_back(path);
        return {};
    }
    ldm::base::Status CreateDirectories(const fs::path&) override
    {
        if (failDirectories) {
            return ldm::base::MakeError(ldm::base::ErrorCode::Io, "access denied");
        }
        return {};
    }
    ldm::base::Status RemoveFolder(const fs::path& path) override
    {
        const std::wstring prefix = ldm::base::PathComparisonKey(path) + L"\\";
        std::erase_if(files, [&prefix](const auto& file) { return file.first.starts_with(prefix); });
        removedFolders.push_back(path);
        return {};
    }
    ldm::base::Status WriteFile(const fs::path& path, std::string_view bytes, bool) override
    {
        Put(path, static_cast<std::int64_t>(bytes.size()));
        return {};
    }

    void Put(const fs::path& path, std::int64_t size) { files[ldm::base::PathComparisonKey(path)] = size; }

    std::map<std::wstring, std::int64_t> files;
    std::vector<fs::path> removed;
    std::vector<fs::path> recycled;
    std::vector<fs::path> removedFolders;
    bool failDirectories = false;
};

class FakeClock final : public ldm::core::Clock {
public:
    std::int64_t NowUnixMs() override { return unixMs; }
    std::chrono::steady_clock::time_point Steady() override { return steady; }
    void Advance(std::chrono::milliseconds by)
    {
        steady += by;
        unixMs += by.count();
    }
    std::int64_t unixMs = 1'800'000'000'000;
    std::chrono::steady_clock::time_point steady = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
};

// Records the site lookups; tests answer them with SiteLookupFinished events.
class FakeSiteResolver final : public ldm::core::SiteResolver {
public:
    void Resolve(ldm::core::SiteLookupRequest request) override { asked.push_back(std::move(request)); }
    void Cancel(std::int64_t lookupId) override { cancelled.push_back(lookupId); }
    std::vector<ldm::core::SiteLookupRequest> asked;
    std::vector<std::int64_t> cancelled;
};

class FakeSecrets final : public ldm::core::SecretProtector {
public:
    ldm::base::Result<std::vector<std::uint8_t>> Protect(std::string_view secret) override
    {
        std::vector<std::uint8_t> bytes{'P'};
        bytes.insert(bytes.end(), secret.begin(), secret.end());
        return bytes;
    }
    ldm::base::Result<std::string> Unprotect(std::span<const std::uint8_t> data) override
    {
        return std::string(data.begin() + 1, data.end());
    }
};

class CountingIds final : public ldm::core::IdGenerator {
public:
    std::string NewPublicId() override { return "public-" + std::to_string(++next); }
    int next = 0;
};

class FakePower final : public ldm::core::PowerRequests {
public:
    void SetTransfersActive(bool value) override { active = value; }
    bool active = false;
};

// Answers at once unless `pending` is set; then it records the lookup and answers nothing.
class FakeProxy final : public ldm::core::ProxyResolver {
public:
    void Configure(const ldm::model::Settings&) override {}
    ldm::core::ProxyLookup Resolve(DownloadId id, std::int64_t epoch, const std::string&) override
    {
        if (pending) {
            lookups.emplace_back(id, epoch);
            return ldm::core::ProxyLookup{true, std::nullopt};
        }
        return ldm::core::ProxyLookup{false, std::nullopt};
    }
    bool pending = false;
    std::vector<std::pair<DownloadId, std::int64_t>> lookups;
};

// Local time is UTC.
class UtcZone final : public ldm::core::TimeZone {
public:
    std::int64_t LocalToUnixMs(std::int64_t localMs) const override { return localMs; }
    std::int64_t UnixToLocalMs(std::int64_t unixMs) const override { return unixMs; }
};

class FakeWakeTimer final : public ldm::core::WakeTimer {
public:
    void Arm(std::int64_t unixMs) override { armed = unixMs; }
    void Disarm() override { armed.reset(); }
    std::optional<std::int64_t> armed;
};

class RecordingObserver final : public ldm::core::CoreObserver {
public:
    void OnSnapshot(std::shared_ptr<const ldm::core::Snapshot> value) override { snapshot = std::move(value); }
    void OnNotification(ldm::core::Notification value) override { notifications.push_back(std::move(value)); }

    template <typename T>
    std::vector<T> All() const
    {
        std::vector<T> result;
        for (const auto& notification : notifications) {
            if (const T* typed = std::get_if<T>(&notification)) {
                result.push_back(*typed);
            }
        }
        return result;
    }

    std::shared_ptr<const ldm::core::Snapshot> snapshot;
    std::vector<ldm::core::Notification> notifications;
};

class CoordinatorTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        auto store = ldm::storage::SqliteStore::Open(":memory:");
        ASSERT_TRUE(store);
        store_ = std::make_unique<ldm::storage::SqliteStore>(std::move(store).Value());
        Recreate();
    }

    // Builds a fresh coordinator over the same store, as a restart of LDM would.
    void Recreate()
    {
        ldm::core::CoordinatorPorts ports;
        ports.store = store_.get();
        ports.engine = &engine_;
        ports.observer = &observer_;
        ports.clock = &clock_;
        ports.fileSystem = &files_;
        ports.secrets = &secrets_;
        ports.ids = &ids_;
        ports.power = &power_;
        ports.proxy = &proxy_;
        ports.media = &media_;
        ports.timeZone = timeZone_;
        ports.wakeTimer = &wakeTimer_;
        ports.sites = &sites_;
        ldm::core::CoordinatorConfig config;
        config.downloadsFolder = kRoot;
        coordinator_ = std::make_unique<Coordinator>(ports, config);
        ASSERT_TRUE(coordinator_->Initialize());
    }

    AddResult Add(const std::string& url, AddMode mode = AddMode::StartNow,
                  std::function<void(AddDownloadCommand&)> customize = {})
    {
        AddDownloadCommand command;
        command.token = ++token_;
        command.url = url;
        command.mode = mode;
        if (customize) {
            customize(command);
        }
        coordinator_->Handle(command);
        const auto results = observer_.All<AddResult>();
        EXPECT_FALSE(results.empty());
        return results.empty() ? AddResult{} : results.back();
    }

    const ldm::core::DownloadView& View(DownloadId id)
    {
        Publish();
        for (const auto& view : observer_.snapshot->downloads) {
            if (view->download.id == id) {
                return *view;
            }
        }
        ADD_FAILURE() << "download " << id.value << " not in the snapshot";
        static const ldm::core::DownloadView empty;
        return empty;
    }

    bool InSnapshot(DownloadId id)
    {
        Publish();
        return std::any_of(observer_.snapshot->downloads.begin(), observer_.snapshot->downloads.end(),
                           [&](const auto& view) { return view->download.id == id; });
    }

    void Publish()
    {
        clock_.Advance(std::chrono::milliseconds(200));
        coordinator_->Tick();
    }

    void Send(DownloadId id, std::int64_t epoch, ldm::core::EnginePayload payload)
    {
        coordinator_->OnEngineEvent(EngineEvent{id, epoch, std::move(payload)});
    }

    void Finish(DownloadId id, std::int64_t epoch, TransferOutcome outcome, std::vector<Segment> segments = {},
                std::optional<TransferFailure> failure = std::nullopt, std::optional<std::int64_t> size = std::nullopt)
    {
        TransferFinished finished;
        finished.outcome = outcome;
        finished.segments = std::move(segments);
        finished.failure = std::move(failure);
        finished.totalSize = size;
        Send(id, epoch, finished);
    }

    std::optional<ldm::model::Download> Stored(DownloadId id)
    {
        auto loaded = store_->LoadDownloads();
        for (auto& download : loaded.Value()) {
            if (download.id == id) {
                return download;
            }
        }
        return std::nullopt;
    }

    std::unique_ptr<ldm::storage::SqliteStore> store_;
    FakeEngine engine_;
    FakeFileSystem files_;
    FakeClock clock_;
    FakeSecrets secrets_;
    CountingIds ids_;
    FakePower power_;
    FakeProxy proxy_;
    FakeMediaTools media_;
    FakeWakeTimer wakeTimer_;
    FakeSiteResolver sites_;
    ldm::core::TimeZone* timeZone_ = nullptr;  // no schedules unless a test sets one
    RecordingObserver observer_;
    std::unique_ptr<Coordinator> coordinator_;
    std::uint64_t token_ = 0;
};

TEST_F(CoordinatorTest, SeedsBuiltinCategoriesOnFirstRun)
{
    ASSERT_TRUE(observer_.snapshot);
    const auto& categories = observer_.snapshot->categories;
    ASSERT_EQ(categories.size(), 6u);
    EXPECT_EQ(categories[0].name, "General");
    EXPECT_EQ(ldm::base::ToUtf16(categories[0].folder), kRoot.wstring());
    EXPECT_EQ(categories[1].name, "Compressed");
    EXPECT_EQ(ldm::base::ToUtf16(categories[1].folder), (kRoot / L"Compressed").wstring());

    Recreate();  // a second start must not seed again
    EXPECT_EQ(observer_.snapshot->categories.size(), 6u);
}

TEST_F(CoordinatorTest, AddRoutesToCategoryAndStartsTheEngine)
{
    const AddResult result = Add("  https://files.test/dir/My%20Archive.zip?x=1  ");
    ASSERT_TRUE(result.id);
    ASSERT_EQ(engine_.starts.size(), 1u);
    const auto& request = engine_.starts.back();
    EXPECT_EQ(request.url, "https://files.test/dir/My%20Archive.zip?x=1");
    EXPECT_EQ(request.referer, "https://files.test/");
    EXPECT_EQ(request.maxConnections, 8);
    EXPECT_EQ(request.epoch, 1);
    EXPECT_EQ(request.partialPath, kRoot / L"Compressed" / L"My Archive.zip.ldmpart");
    EXPECT_FALSE(request.userAgent.empty());
    EXPECT_TRUE(power_.active);

    const auto& view = View(*result.id);
    EXPECT_EQ(view.download.fileName, "My Archive.zip");
    EXPECT_EQ(view.download.status, DownloadStatus::Downloading);
    EXPECT_FALSE(view.download.publicId.empty());
}

TEST_F(CoordinatorTest, RejectsInvalidAddresses)
{
    const AddResult result = Add("file:///C:/Windows/win.ini");
    EXPECT_FALSE(result.id);
    EXPECT_TRUE(result.error);
    EXPECT_TRUE(engine_.starts.empty());
}

TEST_F(CoordinatorTest, DuplicatePolicies)
{
    const AddResult first = Add("http://x.test/a.zip", AddMode::Later);
    ASSERT_TRUE(first.id);

    const AddResult asked = Add("http://x.test/a.zip");
    EXPECT_FALSE(asked.id);
    EXPECT_EQ(asked.duplicateOf, first.id);

    const AddResult numbered = Add("http://x.test/a.zip", AddMode::Later,
                                   [](AddDownloadCommand& c) { c.duplicatePolicy = ldm::model::DuplicatePolicy::AddNumbered; });
    ASSERT_TRUE(numbered.id);
    EXPECT_EQ(View(*numbered.id).download.fileName, "a (2).zip");

    const AddResult resumed = Add("http://x.test/a.zip", AddMode::Later, [](AddDownloadCommand& c) {
        c.duplicatePolicy = ldm::model::DuplicatePolicy::ResumeOrShowComplete;
    });
    EXPECT_EQ(resumed.id, first.id);
    EXPECT_EQ(View(*first.id).download.status, DownloadStatus::Downloading);
}

TEST_F(CoordinatorTest, ExistingFilePolicies)
{
    files_.Put(kRoot / L"Compressed" / L"a.zip", 10);
    const AddResult renamed = Add("http://x.test/a.zip", AddMode::Later);
    EXPECT_EQ(View(*renamed.id).download.fileName, "a (2).zip");

    const AddResult cancelled =
        Add("http://y.test/a.zip", AddMode::Later, [](AddDownloadCommand& c) { c.existingFilePolicy = ldm::core::ExistingFilePolicy::Cancel; });
    EXPECT_FALSE(cancelled.id);
    EXPECT_TRUE(cancelled.error);

    const AddResult replaced =
        Add("http://z.test/a.zip", AddMode::StartNow, [](AddDownloadCommand& c) { c.existingFilePolicy = ldm::core::ExistingFilePolicy::Replace; });
    ASSERT_TRUE(replaced.id);
    EXPECT_EQ(View(*replaced.id).download.fileName, "a.zip");
    // The old file is replaced only when the new one is complete.
    EXPECT_TRUE(files_.Exists(kRoot / L"Compressed" / L"a.zip"));
    Finish(*replaced.id, 1, TransferOutcome::Completed, {{0, 5, 5}}, std::nullopt, 5);
    EXPECT_FALSE(files_.Exists(kRoot / L"Compressed" / L"a.zip"));
    ASSERT_EQ(engine_.finalizes.size(), 1u);
    EXPECT_EQ(engine_.finalizes.back().finalPath, kRoot / L"Compressed" / L"a.zip");
}

TEST_F(CoordinatorTest, FullLifecycleWithServerNameCheckpointsAndCompletion)
{
    const AddResult result = Add("http://x.test/get?id=7");
    const DownloadId id = *result.id;
    EXPECT_EQ(View(id).download.fileName, "get");

    ResponseInfo info;
    info.finalUrl = "http://cdn.x.test/files/report.pdf";
    info.totalSize = 1000;
    info.resumable = true;
    info.validators.etag = "\"abc\"";
    info.fileName = "Report: Q3.pdf";
    Send(id, 1, info);
    EXPECT_EQ(View(id).download.fileName, "Report_ Q3.pdf");
    EXPECT_EQ(View(id).download.totalSize, 1000);
    EXPECT_EQ(Stored(id)->validators.etag, "\"abc\"");

    ProgressInfo progress;
    progress.segments = {{0, 500, 200}, {500, 1000, 100}};
    progress.totalSize = 1000;
    progress.receivedBytes = 400;
    progress.bytesPerSecond = 100;
    progress.resumable = true;
    Send(id, 1, progress);
    EXPECT_EQ(View(id).secondsLeft, 6);
    clock_.Advance(std::chrono::milliseconds(2500));
    coordinator_->Tick();
    EXPECT_EQ(Stored(id)->DownloadedBytes(), 300);  // only durable bytes are stored

    Finish(id, 1, TransferOutcome::Completed, {{0, 500, 500}, {500, 1000, 500}}, std::nullopt, 1000);
    EXPECT_EQ(View(id).download.status, DownloadStatus::Finalizing);
    ASSERT_EQ(engine_.finalizes.size(), 1u);
    const auto& finalize = engine_.finalizes.back();
    EXPECT_EQ(finalize.partialPath, kRoot / L"get.ldmpart");  // the partial keeps its first name
    EXPECT_EQ(finalize.finalPath, kRoot / L"Report_ Q3.pdf");
    EXPECT_FALSE(finalize.lastModified);  // "set file date" is off by default

    Send(id, 1, ldm::core::FinalizeFinished{});
    const auto& view = View(id);
    EXPECT_EQ(view.download.status, DownloadStatus::Completed);
    EXPECT_TRUE(view.download.completedAtMs);
    EXPECT_FALSE(view.download.partialPath);
    EXPECT_FALSE(power_.active);
    const auto completed = observer_.All<ldm::core::DownloadCompleted>();
    ASSERT_EQ(completed.size(), 1u);
    EXPECT_EQ(ldm::base::ToUtf16(completed[0].finalPath), (kRoot / L"Report_ Q3.pdf").wstring());
}

TEST_F(CoordinatorTest, EventsFromOlderRunsAreIgnored)
{
    const DownloadId id = *Add("http://x.test/a.dat").id;
    coordinator_->Handle(ldm::core::StopCommand{{id}});
    Finish(id, 1, TransferOutcome::Stopped, {{0, 100, 40}}, std::nullopt, 100);
    coordinator_->Handle(ldm::core::ResumeCommand{{id}});
    ASSERT_EQ(engine_.starts.size(), 2u);
    EXPECT_EQ(engine_.starts.back().epoch, 2);

    // A late completion of run 1 must not finish run 2.
    Finish(id, 1, TransferOutcome::Completed, {{0, 100, 100}}, std::nullopt, 100);
    EXPECT_EQ(View(id).download.status, DownloadStatus::Downloading);
    EXPECT_TRUE(engine_.finalizes.empty());
}

TEST_F(CoordinatorTest, StopAndResumeCarriesProgress)
{
    const DownloadId id = *Add("http://x.test/a.dat").id;
    coordinator_->Handle(ldm::core::StopCommand{{id}});
    ASSERT_EQ(engine_.stops.size(), 1u);
    EXPECT_EQ(View(id).download.status, DownloadStatus::Stopping);
    coordinator_->Handle(ldm::core::StopCommand{{id}});
    EXPECT_EQ(engine_.stops.size(), 1u) << "a second stop must not be sent";

    Finish(id, 1, TransferOutcome::Stopped, {{0, 100, 40}, {100, 200, 10}}, std::nullopt, 200);
    EXPECT_EQ(View(id).download.status, DownloadStatus::Paused);
    EXPECT_FALSE(power_.active);
    files_.Put(kRoot / L"a.dat.ldmpart", 200);

    coordinator_->Handle(ldm::core::ResumeCommand{{id}});
    ASSERT_EQ(engine_.starts.size(), 2u);
    const auto& resume = engine_.starts.back();
    EXPECT_EQ(resume.segments.size(), 2u);
    EXPECT_EQ(resume.knownSize, 200);
    EXPECT_EQ(resume.partialPath, kRoot / L"a.dat.ldmpart");
}

TEST_F(CoordinatorTest, DeleteWaitsForTheEngineBeforeRemovingFiles)
{
    const DownloadId id = *Add("http://x.test/a.dat").id;
    coordinator_->Handle(ldm::core::DeleteCommand{{id}, true});
    EXPECT_EQ(View(id).download.status, DownloadStatus::Deleting);
    EXPECT_TRUE(files_.removed.empty()) << "the engine may still have the file open";
    ASSERT_EQ(engine_.stops.size(), 1u);

    Finish(id, 1, TransferOutcome::Stopped, {{0, 100, 40}});
    EXPECT_FALSE(InSnapshot(id));
    EXPECT_FALSE(Stored(id));
    ASSERT_EQ(files_.removed.size(), 1u);
    EXPECT_EQ(files_.removed[0], kRoot / L"a.dat.ldmpart");
}

TEST_F(CoordinatorTest, DeleteCompletedKeepsOrRemovesTheFile)
{
    const DownloadId kept = *Add("http://x.test/keep.dat").id;
    Finish(kept, 1, TransferOutcome::Completed, {{0, 1, 1}}, std::nullopt, 1);
    Send(kept, 1, ldm::core::FinalizeFinished{});
    const DownloadId removed = *Add("http://x.test/remove.dat").id;
    Finish(removed, 1, TransferOutcome::Completed, {{0, 1, 1}}, std::nullopt, 1);
    Send(removed, 1, ldm::core::FinalizeFinished{});
    files_.Put(kRoot / L"keep.dat", 1);
    files_.Put(kRoot / L"remove.dat", 1);

    coordinator_->Handle(ldm::core::DeleteCommand{{kept}, false});
    coordinator_->Handle(ldm::core::DeleteCommand{{removed}, true});
    EXPECT_TRUE(files_.Exists(kRoot / L"keep.dat"));
    EXPECT_FALSE(files_.Exists(kRoot / L"remove.dat"));
    ASSERT_EQ(files_.recycled.size(), 1u) << "a deleted completed file goes to the Recycle Bin";
    EXPECT_EQ(files_.recycled[0], kRoot / L"remove.dat");
    EXPECT_FALSE(InSnapshot(kept));
    EXPECT_FALSE(InSnapshot(removed));
}

TEST_F(CoordinatorTest, GrabberDownloadsStayHiddenUntilListed)
{
    ldm::model::GrabberProject project;
    project.name = "Site";
    const auto projectId = store_->InsertGrabberProject(project, "{}");
    ASSERT_TRUE(projectId);
    const DownloadId hidden = *Add("http://x.test/grabbed.dat", AddMode::StartNow, [&](AddDownloadCommand& command) {
                                   command.projectId = projectId.Value();
                                   command.listed = false;
                               }).id;
    EXPECT_FALSE(View(hidden).download.listed);
    EXPECT_EQ(View(hidden).download.projectId, projectId.Value());
    EXPECT_FALSE(View(hidden).download.InMainList());
    EXPECT_FALSE(Stored(hidden)->listed);

    // The same address added by the user is a download of its own, not a duplicate of the hidden one.
    const AddResult own = Add("http://x.test/grabbed.dat", AddMode::Later, [](AddDownloadCommand& command) {
        command.duplicatePolicy = ldm::model::DuplicatePolicy::Ask;
    });
    ASSERT_TRUE(own.id);
    EXPECT_FALSE(own.duplicateOf);
    EXPECT_NE(*own.id, hidden);
    EXPECT_TRUE(View(*own.id).download.InMainList());

    // Delete completed leaves the project's downloads alone.
    Finish(hidden, 1, TransferOutcome::Completed, {{0, 1, 1}}, std::nullopt, 1);
    Send(hidden, 1, ldm::core::FinalizeFinished{});
    const auto completed = observer_.All<ldm::core::DownloadCompleted>();
    ASSERT_EQ(completed.size(), 1u);
    EXPECT_FALSE(completed[0].inMainList) << "the Grabber reports its own downloads";
    coordinator_->Handle(ldm::core::DeleteCompletedCommand{});
    EXPECT_TRUE(InSnapshot(hidden));

    coordinator_->Handle(ldm::core::SetListedCommand{{hidden}});
    EXPECT_TRUE(View(hidden).download.listed);
    EXPECT_TRUE(Stored(hidden)->listed);

    // Only a Grabber download can start hidden.
    const DownloadId plain = *Add("http://x.test/plain.dat", AddMode::Later, [](AddDownloadCommand& command) {
                                  command.listed = false;
                              }).id;
    EXPECT_TRUE(View(plain).download.listed);
}

TEST_F(CoordinatorTest, InconsistentServerRestartsOnceWithOneConnection)
{
    const DownloadId id = *Add("http://x.test/a.dat").id;
    const TransferFailure inconsistent{FailureReason::InconsistentContent, FailureClass::NeedsUser, 0, "overlap"};
    Finish(id, 1, TransferOutcome::Failed, {{0, 100, 50}}, inconsistent, 100);
    ASSERT_EQ(engine_.starts.size(), 2u);
    EXPECT_EQ(engine_.starts.back().maxConnections, 1);
    EXPECT_TRUE(engine_.starts.back().segments.empty()) << "bytes from two answers must never be mixed";

    Finish(id, 2, TransferOutcome::Failed, {{0, 100, 30}}, inconsistent, 100);
    EXPECT_EQ(engine_.starts.size(), 2u);
    EXPECT_EQ(View(id).download.status, DownloadStatus::Failed);
    EXPECT_EQ(observer_.All<ldm::core::DownloadFailed>().size(), 1u);
    EXPECT_TRUE(Stored(id)->segments.empty()) << "the disputed bytes are not kept for a later resume";

    coordinator_->Handle(ldm::core::ResumeCommand{{id}});
    ASSERT_EQ(engine_.starts.size(), 3u);
    EXPECT_TRUE(engine_.starts.back().segments.empty());
    EXPECT_EQ(engine_.starts.back().maxConnections, 1);
}

TEST_F(CoordinatorTest, FailureIsStoredForProperties)
{
    const DownloadId id = *Add("http://x.test/a.dat").id;
    Finish(id, 1, TransferOutcome::Failed, {}, TransferFailure{FailureReason::NotFound, FailureClass::Fatal, 404, "HTTP status 404"});
    EXPECT_EQ(View(id).download.status, DownloadStatus::Failed);
    EXPECT_EQ(Stored(id)->lastResult, "File not found on the server (HTTP status 404)");
    coordinator_->Handle(ldm::core::ResumeCommand{{id}});
    EXPECT_EQ(engine_.starts.size(), 2u) << "a failed download can be retried";
}

TEST_F(CoordinatorTest, RecoversStateAfterACrash)
{
    const DownloadId downloading = *Add("http://x.test/running.dat").id;
    Send(downloading, 1, ProgressInfo{true, {{0, 100, 60}}, 100, 60, 0, {}, std::nullopt});
    clock_.Advance(std::chrono::seconds(3));
    coordinator_->Tick();
    files_.Put(kRoot / L"running.dat.ldmpart", 100);

    const DownloadId lost = *Add("http://x.test/lost.dat").id;
    Send(lost, 1, ProgressInfo{true, {{0, 100, 60}}, 100, 60, 0, {}, std::nullopt});
    clock_.Advance(std::chrono::seconds(3));
    coordinator_->Tick();  // no partial file on disk for this one

    const DownloadId finalizing = *Add("http://x.test/final.dat").id;
    Finish(finalizing, 1, TransferOutcome::Completed, {{0, 100, 100}}, std::nullopt, 100);
    files_.Put(kRoot / L"final.dat", 100);  // the move happened, its commit did not

    const DownloadId deleting = *Add("http://x.test/deleted.dat").id;
    coordinator_->Handle(ldm::core::DeleteCommand{{deleting}, false});

    // Simulated crash: a new coordinator over the same database.
    Recreate();
    EXPECT_EQ(View(downloading).download.status, DownloadStatus::Paused);
    EXPECT_EQ(View(downloading).download.DownloadedBytes(), 60);
    EXPECT_EQ(View(lost).download.status, DownloadStatus::Paused);
    EXPECT_EQ(View(lost).download.DownloadedBytes(), 0) << "progress without its partial file is reset";
    EXPECT_EQ(View(finalizing).download.status, DownloadStatus::Completed);
    EXPECT_FALSE(InSnapshot(deleting));
}

TEST_F(CoordinatorTest, ServerLimitsAndSiteLoginsShapeTheRequest)
{
    coordinator_->Handle(ldm::core::ReplaceServerLimitsCommand{{{"*.slow.test", 2}}});
    coordinator_->Handle(ldm::core::ReplaceSiteLoginsCommand{{{"files.slow.test/private", "alice", "pw"}}});
    Add("http://files.slow.test/private/a.dat");
    const auto& request = engine_.starts.back();
    EXPECT_EQ(request.maxConnections, 2);
    ASSERT_TRUE(request.credentials);
    EXPECT_EQ(request.credentials->user, "alice");
    EXPECT_EQ(request.credentials->password, "pw");

    Add("http://files.slow.test/public/b.dat", AddMode::StartNow,
        [](AddDownloadCommand& c) { c.loginUser = "bob"; c.loginPassword = "secret"; });
    EXPECT_EQ(engine_.starts.back().credentials->user, "bob");
    EXPECT_EQ(engine_.starts.back().credentials->password, "secret");
    const auto stored = Stored(engine_.starts.back().id);
    EXPECT_NE(std::string(stored->loginSecret.begin(), stored->loginSecret.end()), "secret") << "stored protected";
}

TEST_F(CoordinatorTest, BrowserCaptureCarriesCookiesAndUserAgentUntilCompletion)
{
    const ldm::model::Cookie session{"files.test", true, "/", true, 0, "sid", "abc"};
    const ldm::model::Cookie forged{"files.test", true, "/", false, 0, "bad", "x\ny"};
    const DownloadId id = *Add("https://files.test/a.zip", AddMode::StartNow, [&](AddDownloadCommand& c) {
        c.referer = "https://files.test/page";
        c.userAgent = "BrowserUA/1.0";
        c.cookies = {session, forged};
    }).id;
    const auto& request = engine_.starts.back();
    EXPECT_EQ(request.userAgent, "BrowserUA/1.0");
    EXPECT_EQ(request.referer, "https://files.test/page");
    EXPECT_EQ(request.cookieLines, std::vector<std::string>{"files.test\tFALSE\t/\tTRUE\t0\tsid\tabc"})
        << "a cookie with a line break would forge another cookie line; it is dropped";

    const auto stored = Stored(id);
    ASSERT_TRUE(stored);
    EXPECT_FALSE(stored->cookieSecret.empty());
    EXPECT_EQ(stored->cookieSecret.front(), 'P') << "stored through the secret protector (DPAPI in LDM)";
    EXPECT_TRUE(View(id).download.cookieSecret.empty()) << "snapshots never carry secrets";

    Finish(id, 1, TransferOutcome::Completed, {{0, 4, 4}}, std::nullopt, 4);
    Send(id, 1, ldm::core::FinalizeFinished{});
    EXPECT_TRUE(Stored(id)->cookieSecret.empty()) << "cookies are kept only until completion (decision 3)";
}

TEST_F(CoordinatorTest, FormDownloadsUseOneConnectionAndForgetTheFormWhenDone)
{
    const DownloadId id = *Add("https://files.test/get", AddMode::StartNow, [](AddDownloadCommand& c) {
        c.post = ldm::core::PostData{"application/x-www-form-urlencoded", "token=abc"};
    }).id;
    const auto& request = engine_.starts.back();
    ASSERT_TRUE(request.post);
    EXPECT_EQ(request.post->body, "token=abc");
    EXPECT_EQ(request.post->contentType, "application/x-www-form-urlencoded");
    EXPECT_EQ(request.maxConnections, 1);
    EXPECT_EQ(Stored(id)->postSecret.front(), 'P') << "stored through the secret protector";
    EXPECT_TRUE(View(id).download.postSecret.empty()) << "snapshots never carry secrets";

    Finish(id, 1, TransferOutcome::Completed, {{0, 4, 4}}, std::nullopt, 4);
    Send(id, 1, ldm::core::FinalizeFinished{});
    EXPECT_TRUE(Stored(id)->postSecret.empty());
    EXPECT_FALSE(Stored(id)->postContentType);
}

TEST_F(CoordinatorTest, SpeculativeAddFollowsServerNameUntilConfirmed)
{
    const DownloadId id = *Add("http://x.test/download?id=1", AddMode::Speculative).id;
    EXPECT_EQ(engine_.starts.size(), 1u) << "the transfer starts while the dialog is open (A-03)";
    EXPECT_TRUE(View(id).awaitingConfirmation);

    ResponseInfo info;
    info.finalUrl = "http://x.test/download?id=1";
    info.totalSize = 10;
    info.fileName = "movie.mkv";
    Send(id, 1, info);
    EXPECT_EQ(View(id).download.fileName, "movie.mkv");
    EXPECT_EQ(ldm::base::ToUtf16(View(id).download.saveDir), (kRoot / L"Video").wstring());

    // The file completes before the user presses a button: nothing is moved yet.
    Finish(id, 1, TransferOutcome::Completed, {{0, 10, 10}}, std::nullopt, 10);
    EXPECT_TRUE(engine_.finalizes.empty());

    ldm::core::ConfirmDownloadCommand confirm;
    confirm.id = id;
    confirm.fileName = "renamed.mkv";
    confirm.saveDir = ldm::base::ToUtf8((kRoot / L"Elsewhere").wstring());
    confirm.description = "note";
    coordinator_->Handle(confirm);
    // A video is described before it is moved (C-04).
    ASSERT_EQ(media_.describes.size(), 1u);
    EXPECT_TRUE(engine_.finalizes.empty());
    ldm::core::DescribeFinished described;
    described.mediaInfo = "1280x720, 25 fps, MPEG-4; 0:04";
    Send(id, 1, described);
    ASSERT_EQ(engine_.finalizes.size(), 1u);
    EXPECT_EQ(engine_.finalizes.back().finalPath, kRoot / L"Elsewhere" / L"renamed.mkv");
    EXPECT_FALSE(View(id).awaitingConfirmation);
    EXPECT_EQ(View(id).download.description, "note; 1280x720, 25 fps, MPEG-4; 0:04");
}

TEST_F(CoordinatorTest, AFinishedVideoIsDescribedBeforeItIsMovedAndOtherFilesAreNot)
{
    const DownloadId video = *Add("http://x.test/clip.mp4").id;
    Finish(video, 1, TransferOutcome::Completed, {{0, 10, 10}}, std::nullopt, 10);
    ASSERT_EQ(media_.describes.size(), 1u);
    EXPECT_EQ(media_.describes[0].file, ldm::base::ToUtf16(*View(video).download.partialPath))
        << "the complete file before its move";
    EXPECT_EQ(View(video).download.status, DownloadStatus::Finalizing);
    ldm::core::DescribeFinished nothing;  // ffprobe could not say: the file is still fine
    Send(video, 1, nothing);
    ASSERT_EQ(engine_.finalizes.size(), 1u);
    EXPECT_EQ(View(video).download.description, "");

    const DownloadId archive = *Add("http://x.test/files.zip").id;
    Finish(archive, 1, TransferOutcome::Completed, {{0, 10, 10}}, std::nullopt, 10);
    EXPECT_EQ(media_.describes.size(), 1u);
    EXPECT_EQ(engine_.finalizes.size(), 2u);
    const DownloadId subtitles = *Add("http://x.test/film.srt").id;
    Finish(subtitles, 1, TransferOutcome::Completed, {{0, 10, 10}}, std::nullopt, 10);
    EXPECT_EQ(media_.describes.size(), 1u) << "subtitles have nothing to describe";
}

TEST_F(CoordinatorTest, StoppingOrDeletingWhileDescribingKeepsOrRemovesTheFile)
{
    const DownloadId stopped = *Add("http://x.test/a.mp3").id;
    Finish(stopped, 1, TransferOutcome::Completed, {{0, 10, 10}}, std::nullopt, 10);
    coordinator_->Handle(ldm::core::StopCommand{{stopped}});
    ASSERT_EQ(media_.cancels.size(), 1u);
    ldm::core::DescribeFinished cancelled;
    cancelled.stopped = true;
    Send(stopped, 1, cancelled);
    EXPECT_EQ(View(stopped).download.status, DownloadStatus::Paused);
    EXPECT_TRUE(engine_.finalizes.empty());
    coordinator_->Handle(ldm::core::ResumeCommand{{stopped}});
    EXPECT_EQ(engine_.starts.size(), 2u) << "Resume finishes it again";

    const DownloadId deleted = *Add("http://x.test/b.mp3").id;
    Finish(deleted, 1, TransferOutcome::Completed, {{0, 10, 10}}, std::nullopt, 10);
    coordinator_->Handle(ldm::core::DeleteCommand{{deleted}, false});
    EXPECT_TRUE(InSnapshot(deleted)) << "the file is still being read";
    Send(deleted, 1, cancelled);
    EXPECT_FALSE(InSnapshot(deleted));
}

TEST_F(CoordinatorTest, SpeculativeAddKeepsAGivenFolderWhenTheServerNamesTheFile)
{
    const fs::path folder = kRoot / L"Chosen";
    const DownloadId id = *Add("http://x.test/download?id=1", AddMode::Speculative, [&](AddDownloadCommand& c) {
                              c.saveDir = ldm::base::ToUtf8(folder.wstring());
                          }).id;
    ASSERT_EQ(engine_.starts.size(), 1u);
    EXPECT_EQ(engine_.starts.back().partialPath.parent_path(), folder);

    ResponseInfo info;
    info.finalUrl = "http://x.test/download?id=1";
    info.totalSize = 10;
    info.fileName = "movie.mkv";
    Send(id, 1, info);
    EXPECT_EQ(View(id).download.fileName, "movie.mkv");
    EXPECT_EQ(ldm::base::ToUtf16(View(id).download.saveDir), folder.wstring());
}

TEST_F(CoordinatorTest, UnconfirmedSpeculativeAddIsRemovedAfterACrash)
{
    const DownloadId id = *Add("http://x.test/a.iso", AddMode::Speculative).id;
    ASSERT_EQ(engine_.starts.size(), 1u);
    const fs::path partial = engine_.starts.back().partialPath;
    Send(id, 1, ProgressInfo{true, {{0, 100, 30}}, 100, 30, 0, {}, std::nullopt});
    coordinator_->FinalCheckpoint();
    files_.Put(partial, 30);
    ASSERT_TRUE(Stored(id)->awaitingConfirmation);

    Recreate();  // LDM was killed while the info dialog was open
    EXPECT_FALSE(Stored(id));
    EXPECT_FALSE(InSnapshot(id));
    EXPECT_FALSE(files_.Exists(partial));
    EXPECT_TRUE(files_.recycled.empty());
}

TEST_F(CoordinatorTest, ConfirmedSpeculativeAddSurvivesARestart)
{
    const DownloadId id = *Add("http://x.test/a.iso", AddMode::Speculative).id;
    ldm::core::ConfirmDownloadCommand confirm;
    confirm.id = id;
    confirm.fileName = "a.iso";
    confirm.saveDir = View(id).download.saveDir;
    coordinator_->Handle(confirm);
    EXPECT_FALSE(Stored(id)->awaitingConfirmation);

    Recreate();
    ASSERT_TRUE(Stored(id));
    EXPECT_EQ(Stored(id)->status, DownloadStatus::Paused);
}

TEST_F(CoordinatorTest, DownloadLaterAfterSpeculativeStartKeepsBytes)
{
    const DownloadId id = *Add("http://x.test/a.iso", AddMode::Speculative).id;
    ldm::core::ConfirmDownloadCommand confirm;
    confirm.id = id;
    confirm.fileName = "a.iso";
    confirm.saveDir = View(id).download.saveDir;
    confirm.start = false;
    coordinator_->Handle(confirm);
    ASSERT_EQ(engine_.stops.size(), 1u);
    Finish(id, 1, TransferOutcome::Stopped, {{0, 100, 30}}, std::nullopt, 100);
    EXPECT_EQ(View(id).download.status, DownloadStatus::Paused);
    EXPECT_EQ(View(id).download.DownloadedBytes(), 30);
}

TEST_F(CoordinatorTest, ShutdownWaitsForEveryRun)
{
    const DownloadId a = *Add("http://x.test/a.dat").id;
    const DownloadId b = *Add("http://x.test/b.dat").id;
    coordinator_->BeginShutdown();
    EXPECT_EQ(engine_.stops.size(), 2u);
    EXPECT_FALSE(coordinator_->ShutdownComplete());
    Finish(a, 1, TransferOutcome::Stopped, {{0, 10, 5}});
    EXPECT_FALSE(coordinator_->ShutdownComplete());
    Finish(b, 1, TransferOutcome::Failed, {}, TransferFailure{FailureReason::Timeout, FailureClass::Retryable, 0, ""});
    EXPECT_TRUE(coordinator_->ShutdownComplete());
    coordinator_->FinalCheckpoint();
    EXPECT_EQ(Stored(a)->status, DownloadStatus::Paused);
    EXPECT_EQ(Stored(b)->status, DownloadStatus::Paused);

    const AddResult refused = Add("http://x.test/c.dat");
    EXPECT_FALSE(refused.id);
}

TEST_F(CoordinatorTest, SpeedLimiterAndPerDownloadLimit)
{
    coordinator_->Handle(ldm::core::SetSpeedLimiterCommand{true, 300});
    EXPECT_EQ(engine_.globalLimit, 300 * 1024);
    coordinator_->Handle(ldm::core::SetSpeedLimiterCommand{false, std::nullopt});
    EXPECT_FALSE(engine_.globalLimit);

    const DownloadId id = *Add("http://x.test/a.dat").id;
    coordinator_->Handle(ldm::core::SetDownloadSpeedLimitCommand{id, 50'000});
    EXPECT_EQ(engine_.downloadLimit, 50'000);
    EXPECT_EQ(Stored(id)->speedLimitBytesPerSecond, 50'000);
}

TEST_F(CoordinatorTest, RedownloadReplacesTheCompletedFile)
{
    const DownloadId id = *Add("http://x.test/a.dat").id;
    Finish(id, 1, TransferOutcome::Completed, {{0, 4, 4}}, std::nullopt, 4);
    Send(id, 1, ldm::core::FinalizeFinished{});
    files_.Put(kRoot / L"a.dat", 4);

    coordinator_->Handle(ldm::core::RedownloadCommand{id});
    ASSERT_EQ(engine_.starts.size(), 2u);
    EXPECT_TRUE(engine_.starts.back().segments.empty());
    Finish(id, 2, TransferOutcome::Completed, {{0, 4, 4}}, std::nullopt, 4);
    EXPECT_FALSE(files_.Exists(kRoot / L"a.dat")) << "replaced only after the new copy is complete";
    EXPECT_EQ(engine_.finalizes.back().finalPath, kRoot / L"a.dat");
}

TEST_F(CoordinatorTest, SlowProxyLookupDelaysTheEngineWithoutBlocking)
{
    proxy_.pending = true;
    const DownloadId id = *Add("http://x.test/a.dat").id;
    EXPECT_TRUE(engine_.starts.empty()) << "the engine waits for the proxy";
    EXPECT_EQ(View(id).download.status, DownloadStatus::Downloading);
    ASSERT_EQ(proxy_.lookups.size(), 1u);

    Send(id, 0, ldm::core::ProxyResolved{"http://stale.test:1"});
    EXPECT_TRUE(engine_.starts.empty()) << "an answer for another run is ignored";
    Send(id, 1, ldm::core::ProxyResolved{"http://proxy.test:3128"});
    ASSERT_EQ(engine_.starts.size(), 1u);
    EXPECT_EQ(engine_.starts.back().proxy, "http://proxy.test:3128");
}

TEST_F(CoordinatorTest, StopAndDeleteWhileTheProxyIsLookedUp)
{
    proxy_.pending = true;
    const DownloadId stopped = *Add("http://x.test/a.dat").id;
    coordinator_->Handle(ldm::core::StopCommand{{stopped}});
    EXPECT_TRUE(engine_.stops.empty()) << "nothing to stop in the engine";
    EXPECT_EQ(View(stopped).download.status, DownloadStatus::Paused);
    Send(stopped, 1, ldm::core::ProxyResolved{std::nullopt});
    EXPECT_TRUE(engine_.starts.empty()) << "a late answer must not start a stopped run";

    const DownloadId deleted = *Add("http://x.test/b.dat").id;
    coordinator_->Handle(ldm::core::DeleteCommand{{deleted}, true});
    EXPECT_FALSE(InSnapshot(deleted));
    EXPECT_FALSE(Stored(deleted).has_value());
}

void EnableQuota(Coordinator& coordinator, bool warn)
{
    coordinator.Handle(ldm::core::ModifySettingsCommand{[warn](ldm::model::Settings& s) {
        s.quota.enabled = true;
        s.quota.megabytes = 1;
        s.quota.hours = 1;
        s.quota.warnBeforeStopping = warn;
    }});
}

ProgressInfo Received(std::int64_t bytes)
{
    ProgressInfo progress;
    progress.receivedBytes = bytes;
    return progress;
}

TEST_F(CoordinatorTest, QuotaHoldsDownloadsUntilThePeriodEnds)
{
    EnableQuota(*coordinator_, false);
    const DownloadId first = *Add("http://x.test/a.dat").id;
    Send(first, 1, Received(600 * 1024));
    EXPECT_TRUE(engine_.stops.empty());
    Send(first, 1, Received(1100 * 1024));
    ASSERT_EQ(engine_.stops.size(), 1u) << "used up: running downloads stop";
    Finish(first, 1, TransferOutcome::Stopped, {{0, std::nullopt, 1100 * 1024}});
    EXPECT_EQ(View(first).download.status, DownloadStatus::Paused);
    EXPECT_TRUE(View(first).waitingForQuota);
    EXPECT_TRUE(observer_.snapshot->quotaEndsAtUnixMs.has_value());

    // New downloads wait too, and a resume does not get around the quota.
    const DownloadId second = *Add("http://x.test/b.dat").id;
    coordinator_->Handle(ldm::core::ResumeCommand{{first}});
    EXPECT_EQ(engine_.starts.size(), 1u);
    EXPECT_TRUE(View(second).waitingForQuota);

    // Stopped by the user: it stays stopped when the quota allows again.
    coordinator_->Handle(ldm::core::StopCommand{{second}});
    EXPECT_FALSE(View(second).waitingForQuota);

    clock_.Advance(std::chrono::hours(1));
    coordinator_->Tick();
    ASSERT_EQ(engine_.starts.size(), 2u);
    EXPECT_EQ(engine_.starts.back().id, first);
    EXPECT_EQ(engine_.starts.back().segments.size(), 1u) << "it resumes where it stopped";
    EXPECT_FALSE(View(first).waitingForQuota);
}

TEST_F(CoordinatorTest, AFinalNameTakenMeanwhileMovesTheFileToANumberedName)
{
    const DownloadId id = *Add("http://x.test/a.zip").id;
    Finish(id, 1, TransferOutcome::Completed, {{0, 5, 5}}, std::nullopt, 5);
    ASSERT_EQ(engine_.finalizes.size(), 1u);
    const fs::path taken = engine_.finalizes.back().finalPath;
    // Another program saved a file under the same name while the download ran.
    files_.Put(taken, 10);
    ldm::core::FinalizeFinished failed;
    failed.failure = ldm::core::TransferFailure{ldm::core::FailureReason::DiskError, ldm::core::FailureClass::Fatal, 0, "exists"};
    Send(id, 1, failed);
    ASSERT_EQ(engine_.finalizes.size(), 2u);
    EXPECT_EQ(engine_.finalizes.back().finalPath, taken.parent_path() / L"a (2).zip");
    Send(id, 1, ldm::core::FinalizeFinished{});
    EXPECT_EQ(View(id).download.status, DownloadStatus::Completed);
    EXPECT_EQ(View(id).download.fileName, "a (2).zip");
}

TEST_F(CoordinatorTest, StopAllEndsTheWaitForTheQuota)
{
    EnableQuota(*coordinator_, false);
    const DownloadId first = *Add("http://x.test/a.dat").id;
    Send(first, 1, Received(1100 * 1024));
    Finish(first, 1, TransferOutcome::Stopped, {{0, std::nullopt, 1100 * 1024}});
    ASSERT_TRUE(View(first).waitingForQuota);

    coordinator_->Handle(ldm::core::StopAllCommand{});
    EXPECT_FALSE(View(first).waitingForQuota);
    clock_.Advance(std::chrono::hours(1));
    coordinator_->Tick();
    EXPECT_EQ(engine_.starts.size(), 1u) << "stopped by the user: it does not start by itself";
}

TEST_F(CoordinatorTest, QuotaWarningLetsTheUserKeepDownloading)
{
    EnableQuota(*coordinator_, true);
    const DownloadId id = *Add("http://x.test/a.dat").id;
    Send(id, 1, Received(2 * 1024 * 1024));
    EXPECT_TRUE(engine_.stops.empty()) << "nothing stops before the user answers";
    ASSERT_EQ(observer_.All<ldm::core::QuotaReached>().size(), 1u);
    Send(id, 1, Received(3 * 1024 * 1024));
    EXPECT_EQ(observer_.All<ldm::core::QuotaReached>().size(), 1u) << "asked once";

    coordinator_->Handle(ldm::core::QuotaDecisionCommand{true});
    Send(id, 1, Received(4 * 1024 * 1024));
    EXPECT_TRUE(engine_.stops.empty());
    EXPECT_EQ(observer_.All<ldm::core::QuotaReached>().size(), 1u) << "ignored until the period ends";
}

TEST_F(CoordinatorTest, QuotaWarningAnsweredWithStop)
{
    EnableQuota(*coordinator_, true);
    const DownloadId id = *Add("http://x.test/a.dat").id;
    Send(id, 1, Received(2 * 1024 * 1024));
    coordinator_->Handle(ldm::core::QuotaDecisionCommand{false});
    ASSERT_EQ(engine_.stops.size(), 1u);
    Finish(id, 1, TransferOutcome::Stopped, {{0, std::nullopt, 2 * 1024 * 1024}});
    EXPECT_TRUE(View(id).waitingForQuota);
}

TEST_F(CoordinatorTest, NamesStayUniqueAcrossDownloads)
{
    std::set<std::wstring> paths;
    for (int i = 0; i < 5; ++i) {
        const auto result = Add("http://mirror" + std::to_string(i) + ".test/same.zip", AddMode::Later);
        const auto& view = View(*result.id);
        const auto path = fs::path(ldm::base::ToUtf16(view.download.saveDir)) / ldm::base::ToUtf16(view.download.fileName);
        EXPECT_TRUE(paths.insert(ldm::base::PathComparisonKey(path)).second);
    }
    EXPECT_TRUE(paths.contains(ldm::base::PathComparisonKey(kRoot / L"Compressed" / L"same (5).zip")));
}


// ---- streams (M3 design section 6) ----

ldm::media::TrackSegments DashSegments(const std::string& base, int count)
{
    ldm::media::TrackSegments segments;
    segments.inits.push_back(ldm::media::InitSection{base + "init.mp4", std::nullopt});
    for (int i = 0; i < count; ++i) {
        ldm::media::Segment segment;
        segment.url = base + "seg" + std::to_string(i) + ".m4s";
        segment.seconds = 2.0;
        segment.sequence = static_cast<std::uint64_t>(i + 1);
        segment.init = 0;
        segments.segments.push_back(segment);
    }
    return segments;
}

ldm::core::MediaChoice DashChoice()
{
    ldm::core::MediaChoice media;
    media.kind = ldm::model::DownloadKind::Dash;
    ldm::core::MediaTrackChoice video;
    video.role = ldm::model::TrackRole::Video;
    video.sourceUrl = "https://cdn.test/v.mpd";
    video.codecs = "avc1.64001f";
    video.segments = DashSegments("https://cdn.test/v/", 3);
    ldm::core::MediaTrackChoice audio;
    audio.role = ldm::model::TrackRole::Audio;
    audio.sourceUrl = "https://cdn.test/v.mpd";
    audio.codecs = "mp4a.40.2";
    audio.language = "en";
    audio.segments = DashSegments("https://cdn.test/a/", 3);
    media.tracks = {video, audio};
    media.approximateBytes = 3'000'000;
    return media;
}

// A site video as the media panel builds it: whole-file video and audio, each with its format.
ldm::core::MediaChoice SiteChoice()
{
    ldm::core::MediaChoice media;
    media.kind = ldm::model::DownloadKind::Dash;
    media.siteVideo = true;
    for (const char* format : {"299", "140"}) {
        ldm::core::MediaTrackChoice track;
        track.role = std::string_view(format) == "299" ? ldm::model::TrackRole::Video : ldm::model::TrackRole::Audio;
        track.sourceUrl = std::string("https://media.test/videoplayback?itag=") + format;
        ldm::media::TrackSegments whole;
        ldm::media::Segment segment;
        segment.url = track.sourceUrl;
        segment.seconds = 635;
        whole.segments.push_back(segment);
        track.segments = whole;
        track.siteFormat = format;
        track.expectedBytes = std::string_view(format) == "299" ? 257'619'653 : 10'271'496;
        media.tracks.push_back(track);
    }
    return media;
}

TEST_F(CoordinatorTest, ASiteVideoIsStoredWithItsPageAndTrackFormats)
{
    const AddResult result = Add("https://www.youtube.com/watch?v=aqz-KE-bpKQ", AddMode::StartNow,
                                 [](AddDownloadCommand& c) {
                                     c.fileName = "Big Buck Bunny.mp4";
                                     c.media = SiteChoice();
                                 });
    ASSERT_TRUE(result.id) << result.error.value_or("");
    const auto stored = Stored(*result.id);
    ASSERT_TRUE(stored);
    EXPECT_TRUE(stored->siteVideo);
    EXPECT_EQ(stored->kind, ldm::model::DownloadKind::Dash);
    ASSERT_EQ(stored->tracks.size(), 2u);
    EXPECT_TRUE(stored->tracks[0].wholeFile);
    EXPECT_EQ(stored->tracks[0].siteFormat, "299");
    EXPECT_EQ(stored->tracks[1].expectedBytes, 10'271'496);
    ASSERT_EQ(engine_.mediaStarts.size(), 1u);
    EXPECT_TRUE(engine_.mediaStarts[0].tracks[0].wholeFile);
    for (const auto& track : engine_.mediaStarts[0].tracks) {
        EXPECT_EQ(track.maxRequestBytes, ldm::core::kSiteVideoRequestBytes) << "the site slows long requests down";
    }
}

// Fresh addresses (video-sites design, section 8).
class SiteRefreshTest : public CoordinatorTest {
protected:
    // An address of `format` that expires `fromNow` seconds after the fake clock's time.
    std::string Address(const char* format, std::int64_t fromNow) const
    {
        return std::string("https://media.test/videoplayback?itag=") + format +
               "&expire=" + std::to_string(clock_.unixMs / 1000 + fromNow);
    }

    // A site video whose addresses expire `fromNow` seconds from now, added and started.
    DownloadId AddExpiring(std::int64_t fromNow)
    {
        const AddResult result = Add("https://www.youtube.com/watch?v=aqz-KE-bpKQ", AddMode::StartNow,
                                     [this, fromNow](AddDownloadCommand& c) {
                                         c.fileName = "Bunny.mp4";
                                         ldm::core::MediaChoice media = SiteChoice();
                                         for (auto& track : media.tracks) {
                                             track.segments->segments.front().url = Address(track.siteFormat.c_str(), fromNow);
                                         }
                                         c.media = media;
                                     });
        EXPECT_TRUE(result.id) << result.error.value_or("");
        return result.id.value_or(DownloadId{});
    }

    // The resolver's answer to its last question: the formats at new addresses, with `bytes` for 299.
    void Answer(std::int64_t bytes299 = 257'619'653, bool with299 = true, std::vector<std::string> headers = {})
    {
        ASSERT_FALSE(sites_.asked.empty());
        ldm::core::SiteLookupFinished finished;
        finished.lookupId = sites_.asked.back().lookupId;
        ldm::media::SiteVideo video;
        video.id = "aqz-KE-bpKQ";
        for (const char* format : {"299", "140"}) {
            if (std::string_view(format) == "299" && !with299) {
                continue;
            }
            ldm::media::TrackSegments whole;
            ldm::media::Segment segment;
            segment.url = Address(format, 21'600);
            whole.segments.push_back(segment);
            const std::int64_t bytes = std::string_view(format) == "299" ? bytes299 : 10'271'496;
            if (std::string_view(format) == "140") {
                ldm::media::Rendition audio;
                audio.source = whole;
                audio.site = ldm::media::SiteFormat{format, bytes};
                video.offer.audio.push_back(audio);
            } else {
                ldm::media::Variant variant;
                variant.source = whole;
                variant.site = ldm::media::SiteFormat{format, bytes};
                variant.site->headers = headers;
                video.offer.variants.push_back(variant);
            }
        }
        finished.video = video;
        Send(ldm::core::SiteLookupKey(finished.lookupId), 0, finished);
    }
};

TEST_F(SiteRefreshTest, FreshAddressesAreAskedForBeforeTheEngineStartsWhenTheOldOnesExpireSoon)
{
    const DownloadId id = AddExpiring(5 * 60);
    ASSERT_EQ(sites_.asked.size(), 1u);
    EXPECT_EQ(sites_.asked[0].pageUrl, "https://www.youtube.com/watch?v=aqz-KE-bpKQ");
    EXPECT_TRUE(engine_.mediaStarts.empty()) << "not with addresses about to expire";
    EXPECT_EQ(View(id).download.status, DownloadStatus::Downloading);

    Answer();
    ASSERT_EQ(engine_.mediaStarts.size(), 1u);
    EXPECT_EQ(engine_.mediaStarts[0].tracks[0].url, Address("299", 21'600));
    const auto stored = Stored(id);
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->tracks[0].pieces.front().url, Address("299", 21'600)) << "kept for the next start";
}

TEST_F(SiteRefreshTest, FreshAddressesBringTheSitesHeadersForThem)
{
    const DownloadId id = AddExpiring(5 * 60);
    Answer(257'619'653, true, {"X-Playback-Session-Id: s2"});
    ASSERT_EQ(engine_.mediaStarts.size(), 1u);
    EXPECT_EQ(engine_.mediaStarts[0].headers, std::vector<std::string>{"X-Playback-Session-Id: s2"});
    const auto stored = Stored(id);
    ASSERT_TRUE(stored);
    EXPECT_FALSE(stored->siteHeaderSecret.empty()) << "kept for the next start";
}

TEST_F(SiteRefreshTest, AddressesThatStayValidStartAtOnce)
{
    AddExpiring(3 * 3600);
    EXPECT_TRUE(sites_.asked.empty());
    EXPECT_EQ(engine_.mediaStarts.size(), 1u);
}

TEST_F(SiteRefreshTest, StoppingWhileWaitingPausesAtOnceAndTheLateAnswerStartsNothing)
{
    const DownloadId id = AddExpiring(-60);
    ASSERT_EQ(sites_.asked.size(), 1u);
    coordinator_->Handle(ldm::core::StopCommand{{id}});
    EXPECT_EQ(View(id).download.status, DownloadStatus::Paused);
    EXPECT_EQ(sites_.cancelled.size(), 1u);
    Answer();
    EXPECT_TRUE(engine_.mediaStarts.empty());
}

TEST_F(SiteRefreshTest, AFormatThatIsGoneFailsWithTheReason)
{
    const DownloadId id = AddExpiring(-60);
    Answer(257'619'653, false);
    EXPECT_TRUE(engine_.mediaStarts.empty());
    const auto& view = View(id);
    EXPECT_EQ(view.download.status, DownloadStatus::Failed);
    ASSERT_TRUE(view.download.lastResult);
    EXPECT_NE(view.download.lastResult->find("no longer offers"), std::string::npos) << *view.download.lastResult;
}

TEST_F(SiteRefreshTest, AVideoFromAListPicksItsQualityWhenItFirstStarts)
{
    const AddResult result = Add("https://www.youtube.com/watch?v=GvgqDSnpRQM", AddMode::Later, [](AddDownloadCommand& c) {
        c.fileName = "Andrew.mp4";
        ldm::core::MediaChoice media;
        media.siteVideo = true;
        media.siteQuality = ldm::model::SiteQuality::UpTo720;
        c.media = media;
    });
    ASSERT_TRUE(result.id) << result.error.value_or("");
    const DownloadId id = *result.id;
    auto stored = Stored(id);
    ASSERT_TRUE(stored);
    EXPECT_TRUE(stored->tracks.empty()) << "nothing is asked before it starts";
    EXPECT_EQ(stored->siteQuality, ldm::model::SiteQuality::UpTo720);
    EXPECT_TRUE(sites_.asked.empty());

    coordinator_->Handle(ldm::core::ResumeCommand{{id}});
    ASSERT_EQ(sites_.asked.size(), 1u);
    EXPECT_EQ(sites_.asked[0].pageUrl, "https://www.youtube.com/watch?v=GvgqDSnpRQM");
    EXPECT_TRUE(engine_.mediaStarts.empty());

    ldm::core::SiteLookupFinished finished;
    finished.lookupId = sites_.asked.back().lookupId;
    ldm::media::SiteVideo video;
    video.id = "GvgqDSnpRQM";
    video.offer.format = ldm::media::StreamFormat::Hls;
    for (const auto& [format, height] : {std::pair{"137", 1080}, std::pair{"136", 720}}) {
        ldm::media::Variant variant;
        variant.resolution = ldm::media::Resolution{height * 16 / 9, height};
        variant.codecs = "avc1.640028";
        variant.audioGroup = "file";
        ldm::media::TrackSegments whole;
        ldm::media::Segment segment;
        segment.url = Address(format, 21'600);
        whole.segments.push_back(segment);
        variant.source = whole;
        variant.site = ldm::media::SiteFormat{format, std::nullopt};
        video.offer.variants.push_back(variant);
    }
    ldm::media::Rendition audio;
    audio.groupId = "file";
    audio.isDefault = true;
    audio.codecs = "mp4a.40.2";
    ldm::media::TrackSegments whole;
    ldm::media::Segment segment;
    segment.url = Address("140", 21'600);
    whole.segments.push_back(segment);
    audio.source = whole;
    audio.site = ldm::media::SiteFormat{"140", std::nullopt};
    video.offer.audio.push_back(audio);
    finished.video = video;
    Send(ldm::core::SiteLookupKey(finished.lookupId), 0, finished);

    ASSERT_EQ(engine_.mediaStarts.size(), 1u);
    ASSERT_EQ(engine_.mediaStarts[0].tracks.size(), 2u);
    EXPECT_EQ(engine_.mediaStarts[0].tracks[0].url, Address("136", 21'600)) << "the best picture up to 720p";
    EXPECT_EQ(engine_.mediaStarts[0].tracks[1].url, Address("140", 21'600));
    stored = Stored(id);
    ASSERT_TRUE(stored);
    ASSERT_EQ(stored->tracks.size(), 2u);
    EXPECT_EQ(stored->tracks[0].siteFormat, "136");
    EXPECT_EQ(stored->kind, ldm::model::DownloadKind::Dash);
}

TEST_F(SiteRefreshTest, ARefusedHlsTrackReadsItsFreshPlaylistAndKeepsThePiecesOnDisk)
{
    const AddResult result = Add("https://www.youtube.com/watch?v=aqz-KE-bpKQ", AddMode::StartNow, [](AddDownloadCommand& c) {
        c.fileName = "Clip.mp4";
        ldm::core::MediaChoice media;
        media.kind = ldm::model::DownloadKind::Hls;
        media.siteVideo = true;
        ldm::core::MediaTrackChoice video;
        video.sourceUrl = "https://vod.test/old/index.m3u8";
        video.siteFormat = "hls-720";
        media.tracks = {video};
        media.siteHeaders = {"Stream_Key: k1"};
        c.media = media;
    });
    ASSERT_TRUE(result.id) << result.error.value_or("");
    const DownloadId id = *result.id;
    const auto answerPlaylist = [this](const std::string& folder) {
        ASSERT_FALSE(engine_.fetches.empty());
        ldm::core::FetchFinished answer;
        answer.fetchId = engine_.fetches.back().fetchId;
        answer.finalUrl = "https://vod.test/" + folder + "/index.m3u8";
        answer.body = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXTINF:4,\na.ts\n#EXTINF:4,\nb.ts\n#EXT-X-ENDLIST\n";
        Send(DownloadId{}, 0, answer);
    };
    EXPECT_EQ(engine_.fetches.back().headers, std::vector<std::string>{"Stream_Key: k1"}) << "the playlist's request too";
    answerPlaylist("old");
    ASSERT_EQ(engine_.mediaStarts.size(), 1u);

    ldm::core::MediaFinished refused;
    refused.outcome = TransferOutcome::Failed;
    refused.failure = TransferFailure{ldm::core::FailureReason::PermissionDenied, ldm::core::FailureClass::Fatal, 403,
                                      "HTTP status 403"};
    refused.done = {ldm::core::MediaPieceDone{0, 0, 4096}};
    files_.Put(engine_.mediaStarts[0].tracks[0].pieces[0].path, 4096);
    Send(id, engine_.mediaStarts[0].epoch, refused);
    ASSERT_EQ(sites_.asked.size(), 1u);

    ldm::core::SiteLookupFinished finished;
    finished.lookupId = sites_.asked.back().lookupId;
    ldm::media::SiteVideo fresh;
    fresh.id = "aqz-KE-bpKQ";
    ldm::media::Variant variant;
    variant.source = ldm::media::PlaylistAddress{"https://vod.test/new/index.m3u8"};
    variant.site = ldm::media::SiteFormat{"hls-720", std::nullopt};
    fresh.offer.variants.push_back(variant);
    finished.video = fresh;
    const size_t fetchesBefore = engine_.fetches.size();
    Send(ldm::core::SiteLookupKey(finished.lookupId), 0, finished);
    ASSERT_EQ(engine_.fetches.size(), fetchesBefore + 1) << "the fresh playlist is read first";
    EXPECT_EQ(engine_.fetches.back().url, "https://vod.test/new/index.m3u8");
    EXPECT_EQ(engine_.mediaStarts.size(), 1u);

    answerPlaylist("new");
    ASSERT_EQ(engine_.mediaStarts.size(), 2u);
    const auto& pieces = engine_.mediaStarts[1].tracks[0].pieces;
    ASSERT_EQ(pieces.size(), 1u) << "the piece on disk is kept";
    EXPECT_EQ(pieces[0].url, "https://vod.test/new/b.ts");
    const auto stored = Stored(id);
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->tracks[0].sourceUrl, "https://vod.test/new/index.m3u8");
    EXPECT_TRUE(stored->tracks[0].pieces[0].done);
}

TEST_F(CoordinatorTest, AnHlsSiteFormatsParametersGoIntoEveryAddressItsPlaylistLists)
{
    const AddResult result = Add("https://www.loom.com/share/43d05f362f734614a2e81b4694a3a523", AddMode::StartNow,
                                 [](AddDownloadCommand& c) {
                                     c.fileName = "Loom.mp4";
                                     ldm::core::MediaChoice media;
                                     media.kind = ldm::model::DownloadKind::Hls;
                                     media.siteVideo = true;
                                     ldm::core::MediaTrackChoice video;
                                     video.sourceUrl = "https://cdn.test/id/1/hls/media.m3u8?Policy=p&Signature=s";
                                     video.siteFormat = "hls-raw-1500";
                                     video.segmentQuery = "Policy=p&Signature=s";
                                     video.keyQuery = "token=t";
                                     media.tracks = {video};
                                     c.media = media;
                                 });
    ASSERT_TRUE(result.id) << result.error.value_or("");
    ASSERT_FALSE(engine_.fetches.empty());
    ldm::core::FetchFinished answer;
    answer.fetchId = engine_.fetches.back().fetchId;
    answer.body = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXT-X-MAP:URI=\"init.mp4\"\n"
                  "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n#EXTINF:4,\nseg0.m4s?n=0\n#EXTINF:4,\nseg1.m4s\n#EXT-X-ENDLIST\n";
    Send(DownloadId{}, 0, answer);
    ASSERT_EQ(engine_.mediaStarts.size(), 1u);
    const auto& track = engine_.mediaStarts[0].tracks[0];
    ASSERT_EQ(track.pieces.size(), 3u);
    EXPECT_EQ(track.pieces[0].url, "https://cdn.test/id/1/hls/init.mp4?Policy=p&Signature=s");
    EXPECT_EQ(track.pieces[1].url, "https://cdn.test/id/1/hls/seg0.m4s?n=0&Policy=p&Signature=s");
    EXPECT_EQ(track.pieces[2].url, "https://cdn.test/id/1/hls/seg1.m4s?Policy=p&Signature=s");
    ASSERT_EQ(track.keys.size(), 1u);
    EXPECT_EQ(track.keys[0].url, "https://cdn.test/id/1/hls/key.bin?token=t") << "the key's own parameters";
    const auto stored = Stored(*result.id);
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->tracks[0].segmentQuery, "Policy=p&Signature=s") << "for the next reading of the playlist";
}

TEST_F(CoordinatorTest, AddressParametersAreRefusedOutsideAnHlsSiteVideosRules)
{
    const AddResult result = Add("https://www.loom.com/share/x", AddMode::StartNow, [](AddDownloadCommand& c) {
        c.fileName = "Loom.mp4";
        ldm::core::MediaChoice media;
        media.kind = ldm::model::DownloadKind::Hls;
        media.siteVideo = true;
        ldm::core::MediaTrackChoice video;
        video.sourceUrl = "https://cdn.test/media.m3u8";
        video.siteFormat = "hls";
        video.segmentQuery = "a=1#fragment";
        media.tracks = {video};
        c.media = media;
    });
    EXPECT_FALSE(result.id);
}

TEST_F(SiteRefreshTest, ARefusedAddressGetsOneMoreRunWithFreshAddressesThenFails)
{
    const DownloadId id = AddExpiring(3 * 3600);
    ASSERT_EQ(engine_.mediaStarts.size(), 1u);
    const std::int64_t firstEpoch = engine_.mediaStarts[0].epoch;
    ldm::core::MediaFinished refused;
    refused.outcome = TransferOutcome::Failed;
    refused.failure = TransferFailure{ldm::core::FailureReason::PermissionDenied, ldm::core::FailureClass::Fatal, 403,
                                      "HTTP status 403"};
    Send(id, firstEpoch, refused);
    ASSERT_EQ(sites_.asked.size(), 1u) << "asked again although the addresses look valid";
    EXPECT_EQ(View(id).download.status, DownloadStatus::Downloading);
    Answer();
    ASSERT_EQ(engine_.mediaStarts.size(), 2u);

    Send(id, engine_.mediaStarts[1].epoch, refused);
    EXPECT_EQ(sites_.asked.size(), 1u) << "a second refusal in a row is not asked about again";
    EXPECT_EQ(View(id).download.status, DownloadStatus::Failed);
}

TEST_F(CoordinatorTest, OtherStreamsHaveNoRequestCap)
{
    const AddResult result = Add("https://cdn.test/v.mpd", AddMode::StartNow, [](AddDownloadCommand& c) { c.media = DashChoice(); });
    ASSERT_TRUE(result.id);
    ASSERT_EQ(engine_.mediaStarts.size(), 1u);
    for (const auto& track : engine_.mediaStarts[0].tracks) {
        EXPECT_FALSE(track.maxRequestBytes);
    }
}

TEST_F(CoordinatorTest, ASiteVideoNeedsItsCanonicalPageAndAFormatPerTrack)
{
    for (const char* page : {"https://youtu.be/aqz-KE-bpKQ", "https://www.youtube.com/watch?v=aqz-KE-bpKQ&list=PL1",
                             "http://localhost/videoplayback?itag=299"}) {
        const AddResult refused = Add(page, AddMode::StartNow, [](AddDownloadCommand& c) { c.media = SiteChoice(); });
        EXPECT_FALSE(refused.id) << page;
    }
    const AddResult noFormat = Add("https://www.youtube.com/watch?v=aqz-KE-bpKQ", AddMode::StartNow,
                                   [](AddDownloadCommand& c) {
                                       ldm::core::MediaChoice media = SiteChoice();
                                       media.tracks[1].siteFormat.clear();
                                       c.media = media;
                                   });
    EXPECT_FALSE(noFormat.id);
    EXPECT_TRUE(engine_.mediaStarts.empty());
    // A site's DASH piece lists are a site video too, each track with its format.
    const AddResult pieces = Add("https://www.youtube.com/watch?v=aqz-KE-bpKQ", AddMode::StartNow,
                                 [](AddDownloadCommand& c) {
                                     ldm::core::MediaChoice media = DashChoice();
                                     media.siteVideo = true;
                                     for (auto& track : media.tracks) {
                                         track.siteFormat = "1";
                                     }
                                     c.media = media;
                                 });
    ASSERT_TRUE(pieces.id);
    EXPECT_TRUE(View(*pieces.id).download.siteVideo);
}

TEST_F(CoordinatorTest, ADashStreamStartsWithEveryPieceInItsPartsFolder)
{
    const AddResult result = Add("https://cdn.test/v.mpd", AddMode::StartNow, [](AddDownloadCommand& c) {
        c.fileName = "Clip.mp4";
        c.referer = "https://site.test/watch";
        c.media = DashChoice();
    });
    ASSERT_TRUE(result.id) << result.error.value_or("");
    EXPECT_TRUE(engine_.starts.empty()) << "streams are not file transfers";
    ASSERT_EQ(engine_.mediaStarts.size(), 1u);
    const auto& request = engine_.mediaStarts[0];
    EXPECT_EQ(request.referer, "https://site.test/watch");
    EXPECT_TRUE(request.refusePrivateAddresses);
    ASSERT_EQ(request.tracks.size(), 2u);
    ASSERT_EQ(request.tracks[0].pieces.size(), 4u) << "the init section and three segments";
    EXPECT_EQ(request.tracks[0].pieces[0].url, "https://cdn.test/v/init.mp4");
    EXPECT_EQ(request.tracks[0].pieces[1].path, kRoot / L"Video" / L"Clip.mp4.ldmparts" / L"t0" / L"000001.m4s");
    EXPECT_EQ(request.tracks[1].track, 1u);

    const auto stored = Stored(*result.id);
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->kind, ldm::model::DownloadKind::Dash);
    ASSERT_EQ(stored->tracks.size(), 2u);
    EXPECT_EQ(stored->tracks[1].language, "en");
    const auto& view = View(*result.id);
    ASSERT_TRUE(view.stream);
    EXPECT_EQ(view.stream->piecesTotal, 8);
    EXPECT_EQ(view.stream->estimatedBytes, 3'000'000) << "the panel's estimate until pieces arrive";
    EXPECT_TRUE(view.download.tracks[0].pieces.empty()) << "snapshots leave the piece lists out";
}

TEST_F(CoordinatorTest, ASitesOwnHeadersGoWithEveryRequestUntilItCompletes)
{
    const AddResult result = Add("https://cdn.test/v.mpd", AddMode::StartNow, [](AddDownloadCommand& c) {
        c.fileName = "Clip.mp4";
        c.media = DashChoice();
        c.media->siteHeaders = {"Stream_Key: k1", "Host: other.test", "X-Forged: a\nX-More: b"};
    });
    ASSERT_TRUE(result.id) << result.error.value_or("");
    const DownloadId id = *result.id;
    ASSERT_EQ(engine_.mediaStarts.size(), 1u);
    EXPECT_EQ(engine_.mediaStarts[0].headers, std::vector<std::string>{"Stream_Key: k1"})
        << "LDM's own headers and lines that would forge others are left out";
    auto stored = Stored(id);
    ASSERT_TRUE(stored);
    ASSERT_FALSE(stored->siteHeaderSecret.empty());
    EXPECT_EQ(stored->siteHeaderSecret.front(), 'P') << "stored through the secret protector (DPAPI in LDM)";
    EXPECT_TRUE(View(id).download.siteHeaderSecret.empty()) << "snapshots never carry secrets";

    ldm::core::MediaFinished finished;
    finished.outcome = TransferOutcome::Completed;
    finished.done = {{0, 0, 100}, {0, 1, 1000}, {0, 2, 1000}, {0, 3, 1000}, {1, 0, 50}, {1, 1, 500}, {1, 2, 500}, {1, 3, 500}};
    Send(id, 1, finished);
    ldm::core::MuxFinished muxed;
    muxed.output = kRoot / L"Video" / L"Clip.mp4.ldmparts" / L"output.mp4";
    muxed.extension = "mp4";
    muxed.bytes = 4321;
    Send(id, 1, muxed);
    Send(id, 1, ldm::core::FinalizeFinished{});
    ASSERT_EQ(View(id).download.status, DownloadStatus::Completed);
    EXPECT_TRUE(Stored(id)->siteHeaderSecret.empty()) << "kept only until completion, as cookies are";
}

TEST_F(CoordinatorTest, AStreamIsJoinedRenamedDescribedAndMovedWhenItsPiecesAreDone)
{
    const AddResult result = Add("https://cdn.test/v.mpd", AddMode::StartNow, [](AddDownloadCommand& c) {
        c.fileName = "Clip.mp4";
        c.media = DashChoice();
    });
    ASSERT_TRUE(result.id);
    const DownloadId id = *result.id;
    ldm::core::MediaProgress progress;
    progress.done = {{0, 0, 100}, {0, 1, 1000}};
    progress.receivedBytes = 1100;
    Send(id, 1, progress);
    clock_.Advance(std::chrono::seconds(3));
    Send(id, 1, progress);  // a checkpoint is due; the same pieces again change nothing
    auto stored = Stored(id);
    ASSERT_TRUE(stored);
    EXPECT_TRUE(stored->tracks[0].pieces[1].done);
    EXPECT_EQ(stored->tracks[0].pieces[1].bytes, 1000);

    ldm::core::MediaFinished finished;
    finished.outcome = TransferOutcome::Completed;
    finished.done = {{0, 2, 1000}, {0, 3, 1000}, {1, 0, 50}, {1, 1, 500}, {1, 2, 500}, {1, 3, 500}};
    Send(id, 1, finished);
    ASSERT_EQ(media_.muxes.size(), 1u);
    const auto& mux = media_.muxes[0];
    EXPECT_EQ(mux.preferredExtension, "mp4");
    ASSERT_EQ(mux.inputs.size(), 2u);
    ASSERT_EQ(mux.inputs[0].pieces.size(), 4u);
    EXPECT_TRUE(mux.inputs[0].pieces[0].init);
    EXPECT_EQ(mux.inputs[0].pieces[2].initPiece, 0u);
    EXPECT_EQ(mux.inputs[0].pieces[2].durationUs, 2'000'000);
    EXPECT_EQ(View(id).download.status, DownloadStatus::Finalizing);
    EXPECT_TRUE(engine_.finalizes.empty()) << "nothing moves before the pieces are joined";

    ldm::core::MuxFinished muxed;
    muxed.output = kRoot / L"Video" / L"Clip.mp4.ldmparts" / L"output.mkv";
    muxed.extension = "mkv";
    muxed.bytes = 4321;
    muxed.mediaInfo = "320x240, 25 fps, MPEG-4; AAC, stereo";
    Send(id, 1, muxed);
    ASSERT_EQ(engine_.finalizes.size(), 1u);
    const auto& finalize = engine_.finalizes[0];
    EXPECT_EQ(finalize.partialPath, muxed.output);
    EXPECT_EQ(finalize.finalPath, kRoot / L"Video" / L"Clip.mkv");
    EXPECT_EQ(finalize.removeFolder, kRoot / L"Video" / L"Clip.mp4.ldmparts");

    Send(id, 1, ldm::core::FinalizeFinished{});
    const auto& view = View(id);
    EXPECT_EQ(view.download.status, DownloadStatus::Completed);
    EXPECT_EQ(view.download.fileName, "Clip.mkv");
    EXPECT_EQ(view.download.totalSize, 4321);
    EXPECT_EQ(view.download.description, "320x240, 25 fps, MPEG-4; AAC, stereo");
    ASSERT_TRUE(view.stream);
    EXPECT_DOUBLE_EQ(view.stream->fraction, 1.0);
    const auto completed = observer_.All<ldm::core::DownloadCompleted>();
    ASSERT_EQ(completed.size(), 1u);
    EXPECT_EQ(completed[0].size, 4321);
}

TEST_F(CoordinatorTest, AnHlsStreamReadsItsPlaylistsBeforeTheRun)
{
    proxy_.pending = false;
    const AddResult result = Add("https://cdn.test/master.m3u8", AddMode::StartNow, [](AddDownloadCommand& c) {
        c.fileName = "Show.mp4";
        ldm::core::MediaChoice media;
        media.kind = ldm::model::DownloadKind::Hls;
        ldm::core::MediaTrackChoice video;
        video.sourceUrl = "https://cdn.test/v/index.m3u8";
        media.tracks = {video};
        c.media = media;
    });
    ASSERT_TRUE(result.id);
    const DownloadId id = *result.id;
    EXPECT_TRUE(engine_.mediaStarts.empty());
    ASSERT_EQ(engine_.fetches.size(), 1u);
    const auto fetch = engine_.fetches[0];
    EXPECT_LT(fetch.fetchId, 0) << "the media panel's fetch ids are positive";
    EXPECT_EQ(fetch.url, "https://cdn.test/v/index.m3u8");
    EXPECT_TRUE(fetch.refusePrivateAddresses);

    ldm::core::FetchFinished answer;
    answer.fetchId = fetch.fetchId;
    answer.finalUrl = "https://cdn.test/v/index.m3u8";
    answer.body = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXTINF:4,\na.ts\n#EXTINF:4,\nb.ts\n#EXT-X-ENDLIST\n";
    Send(DownloadId{}, 0, answer);
    ASSERT_EQ(engine_.mediaStarts.size(), 1u);
    ASSERT_EQ(engine_.mediaStarts[0].tracks[0].pieces.size(), 2u);
    EXPECT_EQ(engine_.mediaStarts[0].tracks[0].pieces[1].url, "https://cdn.test/v/b.ts");
    const auto stored = Stored(id);
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->tracks[0].pieces.size(), 2u) << "the list is stored once read";

    Send(DownloadId{}, 0, answer);  // a second answer for the same fetch starts nothing
    EXPECT_EQ(engine_.mediaStarts.size(), 1u);
}

// ---- live recordings ----

// A live playlist of two-second segments `first` to `first + count - 1`.
std::string LivePlaylist(int first, int count, bool ended = false)
{
    std::string text = "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:" + std::to_string(first) + "\n";
    for (int i = first; i < first + count; ++i) {
        text += "#EXTINF:2,\nseg" + std::to_string(i) + ".ts\n";
    }
    return ended ? text + "#EXT-X-ENDLIST\n" : text;
}

class LiveRecordingTest : public CoordinatorTest {
protected:
    // Adds a live stream and answers its first playlist with segments 10 to 12.
    DownloadId StartRecording()
    {
        const AddResult result = Add("https://cdn.test/live/master.m3u8", AddMode::StartNow, [](AddDownloadCommand& c) {
            c.fileName = "Live.mp4";
            ldm::core::MediaChoice media;
            ldm::core::MediaTrackChoice video;
            video.sourceUrl = "https://cdn.test/live/index.m3u8";
            media.tracks = {video};
            c.media = media;
        });
        EXPECT_TRUE(result.id) << result.error.value_or("");
        Answer(LivePlaylist(10, 3));
        return result.id.value_or(DownloadId{});
    }

    // Answers the latest playlist fetch.
    void Answer(const std::string& body)
    {
        ASSERT_FALSE(engine_.fetches.empty());
        ldm::core::FetchFinished answer;
        answer.fetchId = engine_.fetches.back().fetchId;
        answer.finalUrl = "https://cdn.test/live/index.m3u8";
        answer.body = body;
        Send(DownloadId{}, 0, answer);
    }

    void FailFetch()
    {
        ASSERT_FALSE(engine_.fetches.empty());
        ldm::core::FetchFinished answer;
        answer.fetchId = engine_.fetches.back().fetchId;
        answer.failure = TransferFailure{FailureReason::HttpError, FailureClass::Retryable, 503, "busy"};
        Send(DownloadId{}, 0, answer);
    }

    // Lets `by` pass and lets the coordinator act on it, without publishing a snapshot.
    void Wait(std::chrono::milliseconds by)
    {
        clock_.Advance(by);
        coordinator_->Tick();
    }
};

TEST_F(LiveRecordingTest, ALiveStreamIsRecordedAndItsPlaylistReadAgainUntilItEnds)
{
    const DownloadId id = StartRecording();
    ASSERT_EQ(engine_.mediaStarts.size(), 1u);
    const auto& start = engine_.mediaStarts[0];
    ASSERT_EQ(start.tracks.size(), 1u);
    EXPECT_TRUE(start.tracks[0].live);
    EXPECT_EQ(start.tracks[0].pieces.size(), 3u);
    EXPECT_TRUE(Stored(id)->tracks[0].live);
    EXPECT_LE(coordinator_->NextDeadline(), clock_.Steady() + std::chrono::seconds(2)) << "the core wakes for it";

    Wait(std::chrono::milliseconds(1900));
    EXPECT_EQ(engine_.fetches.size(), 1u) << "not before one target duration";
    Wait(std::chrono::milliseconds(100));
    ASSERT_EQ(engine_.fetches.size(), 2u);
    EXPECT_EQ(engine_.fetches[1].url, "https://cdn.test/live/index.m3u8");

    Answer(LivePlaylist(11, 3));
    ASSERT_EQ(engine_.extensions.size(), 1u);
    EXPECT_EQ(engine_.extensions[0].first, 1);
    const auto& added = engine_.extensions[0].second.tracks;
    ASSERT_EQ(added.size(), 1u);
    EXPECT_TRUE(added[0].live);
    ASSERT_EQ(added[0].pieces.size(), 1u);
    EXPECT_EQ(added[0].pieces[0].url, "https://cdn.test/live/seg13.ts");
    EXPECT_EQ(added[0].pieces[0].index, 3u);
    EXPECT_EQ(added[0].pieces[0].path, kRoot / L"Video" / L"Live.mp4.ldmparts" / L"t0" / L"000003.ts");
    EXPECT_EQ(Stored(id)->tracks[0].pieces.size(), 4u) << "the new piece is stored as it comes";

    const auto& view = View(id);
    ASSERT_TRUE(view.stream);
    EXPECT_TRUE(view.stream->live);
    EXPECT_FALSE(view.secondsLeft) << "a recording has no end to count down to";

    Wait(std::chrono::seconds(2));
    ASSERT_EQ(engine_.fetches.size(), 3u);
    Answer(LivePlaylist(12, 3, true));
    ASSERT_EQ(engine_.extensions.size(), 2u);
    EXPECT_FALSE(engine_.extensions[1].second.tracks[0].live) << "the stream ended";
    EXPECT_EQ(engine_.extensions[1].second.tracks[0].pieces.size(), 1u);
    EXPECT_FALSE(Stored(id)->tracks[0].live);
    Wait(std::chrono::seconds(10));
    EXPECT_EQ(engine_.fetches.size(), 3u) << "an ended stream is not read again";

    ldm::core::MediaFinished finished;
    finished.outcome = TransferOutcome::Completed;
    finished.done = {{0, 0, 10}, {0, 1, 10}, {0, 2, 10}, {0, 3, 10}, {0, 4, 10}};
    Send(id, 1, finished);
    ASSERT_EQ(media_.muxes.size(), 1u) << "it is joined without a question";
    EXPECT_EQ(media_.muxes[0].inputs[0].pieces.size(), 5u);
}

TEST_F(LiveRecordingTest, APlaylistWithNothingNewIsReadAgainSooner)
{
    StartRecording();
    Wait(std::chrono::seconds(2));
    ASSERT_EQ(engine_.fetches.size(), 2u);
    Answer(LivePlaylist(10, 3));
    EXPECT_TRUE(engine_.extensions.empty());
    Wait(std::chrono::milliseconds(1000));
    EXPECT_EQ(engine_.fetches.size(), 3u) << "half the target duration";
}

TEST_F(LiveRecordingTest, SegmentsMissedInBetweenAreMarkedAsAGap)
{
    const DownloadId id = StartRecording();
    Wait(std::chrono::seconds(2));
    Answer(LivePlaylist(20, 3));
    const auto stored = Stored(id);
    ASSERT_EQ(stored->tracks[0].pieces.size(), 6u);
    EXPECT_TRUE(stored->tracks[0].pieces[3].discontinuity);
    EXPECT_FALSE(stored->tracks[0].pieces[4].discontinuity);
}

TEST_F(LiveRecordingTest, FinishingARunningRecordingJoinsWhatIsOnDisk)
{
    const DownloadId id = StartRecording();
    ldm::core::MediaProgress progress;
    progress.done = {{0, 0, 100}};
    Send(id, 1, progress);
    coordinator_->Handle(ldm::core::FinishRecordingCommand{{id}});
    ASSERT_EQ(engine_.stops.size(), 1u);
    ldm::core::MediaFinished stopped;
    stopped.outcome = TransferOutcome::Stopped;
    stopped.done = {{0, 1, 100}};
    Send(id, 1, stopped);

    ASSERT_EQ(media_.muxes.size(), 1u);
    ASSERT_EQ(media_.muxes[0].inputs[0].pieces.size(), 2u) << "the piece not fetched is left out";
    const auto stored = Stored(id);
    EXPECT_FALSE(stored->tracks[0].live);
    EXPECT_TRUE(stored->tracks[0].pieces[2].skipped);
    EXPECT_EQ(View(id).download.status, DownloadStatus::Finalizing);
    Wait(std::chrono::seconds(10));
    EXPECT_EQ(engine_.fetches.size(), 1u) << "a finished recording is not read again";
}

TEST_F(LiveRecordingTest, FinishingBeforeAnythingWasRecordedOnlyStops)
{
    const DownloadId id = StartRecording();
    coordinator_->Handle(ldm::core::FinishRecordingCommand{{id}});
    ldm::core::MediaFinished stopped;
    stopped.outcome = TransferOutcome::Stopped;
    Send(id, 1, stopped);
    EXPECT_TRUE(media_.muxes.empty());
    const auto& view = View(id);
    EXPECT_EQ(view.download.status, DownloadStatus::Paused);
    EXPECT_EQ(view.download.lastResult, "Nothing was recorded yet");
    EXPECT_TRUE(Stored(id)->tracks[0].live) << "Resume records on";
}

TEST_F(LiveRecordingTest, FinishingAPausedRecordingJoinsItAtOnce)
{
    const DownloadId id = StartRecording();
    coordinator_->Handle(ldm::core::StopCommand{{id}});
    ldm::core::MediaFinished stopped;
    stopped.outcome = TransferOutcome::Stopped;
    stopped.done = {{0, 0, 100}};
    Send(id, 1, stopped);
    EXPECT_EQ(View(id).download.status, DownloadStatus::Paused);
    EXPECT_TRUE(media_.muxes.empty()) << "a plain Stop keeps recording later";

    coordinator_->Handle(ldm::core::FinishRecordingCommand{{id}});
    ASSERT_EQ(media_.muxes.size(), 1u);
    EXPECT_EQ(media_.muxes[0].inputs[0].pieces.size(), 1u);
}

TEST_F(LiveRecordingTest, ResumingAPausedRecordingContinuesAtTheLiveEdge)
{
    const DownloadId id = StartRecording();
    coordinator_->Handle(ldm::core::StopCommand{{id}});
    ldm::core::MediaFinished stopped;
    stopped.outcome = TransferOutcome::Stopped;
    stopped.done = {{0, 0, 100}};
    Send(id, 1, stopped);

    coordinator_->Handle(ldm::core::ResumeCommand{{id}});
    ASSERT_EQ(engine_.mediaStarts.size(), 2u);
    EXPECT_TRUE(engine_.mediaStarts[1].tracks[0].pieces.empty()) << "what it listed before is gone by now";
    EXPECT_TRUE(engine_.mediaStarts[1].tracks[0].live);
    EXPECT_TRUE(Stored(id)->tracks[0].pieces[2].skipped);
    coordinator_->Tick();
    ASSERT_EQ(engine_.fetches.size(), 2u) << "the playlist is read again at once";

    Answer(LivePlaylist(40, 3));
    ASSERT_EQ(engine_.extensions.size(), 1u);
    EXPECT_EQ(engine_.extensions[0].first, 2);
    EXPECT_EQ(engine_.extensions[0].second.tracks[0].pieces.size(), 3u);
    EXPECT_TRUE(Stored(id)->tracks[0].pieces[3].discontinuity);
}

TEST_F(LiveRecordingTest, ARecordingWhosePlaylistKeepsFailingFails)
{
    const DownloadId id = StartRecording();
    for (int i = 0; i < 5; ++i) {
        Wait(std::chrono::seconds(2));
        FailFetch();
    }
    EXPECT_TRUE(engine_.stops.empty()) << "five failures in a row are survived";
    Wait(std::chrono::seconds(2));
    FailFetch();
    ASSERT_EQ(engine_.stops.size(), 1u);
    ldm::core::MediaFinished stopped;
    stopped.outcome = TransferOutcome::Stopped;
    stopped.done = {{0, 0, 100}};
    Send(id, 1, stopped);
    EXPECT_EQ(View(id).download.status, DownloadStatus::Failed);
    ASSERT_EQ(observer_.All<ldm::core::DownloadFailed>().size(), 1u);
    EXPECT_TRUE(Stored(id)->tracks[0].pieces[0].done) << "what was recorded stays";
}

TEST_F(LiveRecordingTest, ASuccessfulReadResetsTheFailureCount)
{
    StartRecording();
    for (int i = 0; i < 5; ++i) {
        Wait(std::chrono::seconds(2));
        FailFetch();
    }
    Wait(std::chrono::seconds(2));
    Answer(LivePlaylist(10, 3));
    for (int i = 0; i < 5; ++i) {
        Wait(std::chrono::seconds(2));
        FailFetch();
    }
    EXPECT_TRUE(engine_.stops.empty());
}

TEST_F(LiveRecordingTest, APlaylistAnswerForAnEarlierRunIsDropped)
{
    const DownloadId id = StartRecording();
    Wait(std::chrono::seconds(2));
    ASSERT_EQ(engine_.fetches.size(), 2u);
    const std::int64_t earlier = engine_.fetches.back().fetchId;
    coordinator_->Handle(ldm::core::StopCommand{{id}});
    ldm::core::MediaFinished stopped;
    stopped.outcome = TransferOutcome::Stopped;
    Send(id, 1, stopped);
    coordinator_->Handle(ldm::core::ResumeCommand{{id}});

    ldm::core::FetchFinished late;
    late.fetchId = earlier;
    late.finalUrl = "https://cdn.test/live/index.m3u8";
    late.body = LivePlaylist(11, 3);
    Send(DownloadId{}, 0, late);
    EXPECT_TRUE(engine_.extensions.empty());
}

TEST_F(LiveRecordingTest, ALiveDashStreamIsRefusedWithTheReason)
{
    const AddResult result = Add("https://cdn.test/v.mpd", AddMode::StartNow, [](AddDownloadCommand& c) {
        ldm::core::MediaChoice media = DashChoice();
        media.tracks[0].segments->live = true;
        c.media = media;
    });
    EXPECT_FALSE(result.id);
    ASSERT_TRUE(result.error);
    EXPECT_NE(result.error->find("live DASH"), std::string::npos) << *result.error;
}

TEST_F(CoordinatorTest, StoppingWhilePlaylistsAreFetchedPausesAtOnce)
{
    const AddResult result = Add("https://cdn.test/m.m3u8", AddMode::StartNow, [](AddDownloadCommand& c) {
        ldm::core::MediaChoice media;
        ldm::core::MediaTrackChoice video;
        video.sourceUrl = "https://cdn.test/v.m3u8";
        media.tracks = {video};
        c.media = media;
    });
    ASSERT_TRUE(result.id);
    ASSERT_EQ(engine_.fetches.size(), 1u);
    coordinator_->Handle(ldm::core::StopCommand{{*result.id}});
    EXPECT_EQ(View(*result.id).download.status, DownloadStatus::Paused);

    ldm::core::FetchFinished late;
    late.fetchId = engine_.fetches[0].fetchId;
    late.body = "#EXTM3U\n#EXTINF:4,\na.ts\n#EXT-X-ENDLIST\n";
    late.finalUrl = "https://cdn.test/v.m3u8";
    Send(DownloadId{}, 0, late);
    EXPECT_TRUE(engine_.mediaStarts.empty()) << "a late playlist starts no run";
}

TEST_F(CoordinatorTest, APublicStreamThatPointsIntoTheHomeNetworkIsRefused)
{
    const AddResult refused = Add("https://cdn.test/v.mpd", AddMode::StartNow, [](AddDownloadCommand& c) {
        ldm::core::MediaChoice media = DashChoice();
        media.tracks[0].segments->segments[1].url = "http://192.168.1.1/admin";
        c.media = media;
    });
    EXPECT_FALSE(refused.id);
    EXPECT_TRUE(refused.error);
    EXPECT_TRUE(engine_.mediaStarts.empty());

    // The same stream from a private host may point at private hosts.
    const AddResult local = Add("http://192.168.1.20/v.mpd", AddMode::StartNow, [](AddDownloadCommand& c) {
        ldm::core::MediaChoice media = DashChoice();
        media.tracks[0].segments->segments[1].url = "http://192.168.1.1/seg.m4s";
        c.media = media;
    });
    ASSERT_TRUE(local.id) << local.error.value_or("");
    ASSERT_EQ(engine_.mediaStarts.size(), 1u);
    EXPECT_FALSE(engine_.mediaStarts[0].refusePrivateAddresses);
}

TEST_F(CoordinatorTest, AStoppedStreamResumesWithOnlyItsMissingPieces)
{
    const AddResult result = Add("https://cdn.test/v.mpd", AddMode::StartNow, [](AddDownloadCommand& c) {
        c.fileName = "Clip.mp4";
        c.media = DashChoice();
    });
    ASSERT_TRUE(result.id);
    const DownloadId id = *result.id;
    coordinator_->Handle(ldm::core::StopCommand{{id}});
    ldm::core::MediaFinished stopped;
    stopped.outcome = TransferOutcome::Stopped;
    stopped.done = {{0, 0, 100}, {0, 1, 1000}, {1, 0, 50}};
    Send(id, 1, stopped);
    EXPECT_EQ(View(id).download.status, DownloadStatus::Paused);

    // A restart: the files of two done pieces are there, one is gone.
    const fs::path parts = kRoot / L"Video" / L"Clip.mp4.ldmparts";
    files_.Put(parts / L"t0" / L"000000.mp4", 100);
    files_.Put(parts / L"t0" / L"000001.m4s", 1000);
    Recreate();
    coordinator_->Handle(ldm::core::ResumeCommand{{id}});
    ASSERT_EQ(engine_.mediaStarts.size(), 2u);
    const auto& again = engine_.mediaStarts[1];
    EXPECT_EQ(again.epoch, 2);
    EXPECT_EQ(again.tracks[0].pieces.size(), 2u) << "pieces 2 and 3 of the video";
    EXPECT_EQ(again.tracks[1].pieces.size(), 4u) << "the audio init section's file was gone";
}

TEST_F(CoordinatorTest, StoppingWhileJoiningKeepsThePiecesAndJoinsOnResume)
{
    const AddResult result = Add("https://cdn.test/v.mpd", AddMode::StartNow, [](AddDownloadCommand& c) {
        c.media = DashChoice();
    });
    ASSERT_TRUE(result.id);
    const DownloadId id = *result.id;
    ldm::core::MediaFinished finished;
    finished.outcome = TransferOutcome::Completed;
    Send(id, 1, finished);
    ASSERT_EQ(media_.muxes.size(), 1u);
    coordinator_->Handle(ldm::core::StopCommand{{id}});
    ASSERT_EQ(media_.cancels.size(), 1u);
    ldm::core::MuxFinished cancelled;
    cancelled.stopped = true;
    Send(id, 1, cancelled);
    EXPECT_EQ(View(id).download.status, DownloadStatus::Paused);
    EXPECT_TRUE(engine_.finalizes.empty());

    coordinator_->Handle(ldm::core::ResumeCommand{{id}});
    EXPECT_EQ(engine_.mediaStarts.size(), 2u);
}

TEST_F(CoordinatorTest, DeletingAStreamRemovesItsPartsFolder)
{
    const AddResult result = Add("https://cdn.test/v.mpd", AddMode::StartNow, [](AddDownloadCommand& c) {
        c.fileName = "Clip.mp4";
        c.media = DashChoice();
    });
    ASSERT_TRUE(result.id);
    coordinator_->Handle(ldm::core::DeleteCommand{{*result.id}, false});
    ldm::core::MediaFinished stopped;
    stopped.outcome = TransferOutcome::Stopped;
    Send(*result.id, 1, stopped);
    EXPECT_FALSE(InSnapshot(*result.id));
    ASSERT_EQ(files_.removedFolders.size(), 1u);
    EXPECT_EQ(files_.removedFolders[0], kRoot / L"Video" / L"Clip.mp4.ldmparts");
}

TEST_F(CoordinatorTest, AStreamsSubtitlesGoWhereItsVideoGoes)
{
    const AddResult result = Add("https://cdn.test/master.m3u8", AddMode::Later, [](AddDownloadCommand& c) {
        c.fileName = "Show.en.vtt";
        ldm::core::MediaChoice media;
        ldm::core::MediaTrackChoice subtitles;
        subtitles.role = ldm::model::TrackRole::Subtitles;
        subtitles.sourceUrl = "https://cdn.test/subs.m3u8";
        media.tracks = {subtitles};
        c.media = media;
    });
    ASSERT_TRUE(result.id) << result.error.value_or("");
    const auto& view = View(*result.id);
    EXPECT_EQ(view.download.fileName, "Show.en.vtt");
    EXPECT_EQ(ldm::base::ToUtf16(view.download.saveDir), (kRoot / L"Video").wstring());
}

TEST_F(CoordinatorTest, ASecondQualityOfTheSameStreamIsNotADuplicate)
{
    ASSERT_TRUE(Add("https://cdn.test/v.mpd", AddMode::Later, [](AddDownloadCommand& c) { c.media = DashChoice(); }).id);
    const AddResult same = Add("https://cdn.test/v.mpd", AddMode::Later, [](AddDownloadCommand& c) { c.media = DashChoice(); });
    EXPECT_TRUE(same.duplicateOf);
    const AddResult other = Add("https://cdn.test/v.mpd", AddMode::Later, [](AddDownloadCommand& c) {
        ldm::core::MediaChoice media = DashChoice();
        media.tracks[0].sourceUrl = "https://cdn.test/v.mpd#1080";
        c.media = media;
    });
    EXPECT_TRUE(other.id);
    EXPECT_FALSE(other.duplicateOf);
}

}  // namespace

TEST_F(CoordinatorTest, BrowserContentTypeMakesAWebPageInsteadOfTheFileAFailure)
{
    Add("https://files.test/get?id=1", AddMode::StartNow, [](AddDownloadCommand& c) { c.contentType = "application/zip"; });
    EXPECT_EQ(engine_.starts.back().knownContentType, "application/zip");
    Add("https://files.test/get?id=2", AddMode::StartNow, [](AddDownloadCommand& c) { c.contentType = "Text/HTML; charset=utf-8"; });
    EXPECT_FALSE(engine_.starts.back().knownContentType) << "a page the browser downloaded is not held against a page";
    Add("https://files.test/get?id=3", AddMode::StartNow, [](AddDownloadCommand& c) { c.contentType = "a\r\nb"; });
    EXPECT_FALSE(engine_.starts.back().knownContentType);
}

TEST_F(CoordinatorTest, WaitingForANewAddressStopsAndTheNewAddressResumes)
{
    const DownloadId id = *Add("https://files.test/old?token=1").id;
    coordinator_->Handle(ldm::core::WaitForAddressCommand{id});
    ASSERT_EQ(engine_.stops.size(), 1u) << "a running download stops while it waits";
    EXPECT_TRUE(View(id).waitingForAddress);
    Finish(id, 1, TransferOutcome::Stopped, {{0, 100, 40}}, std::nullopt, 100);
    files_.Put(kRoot / L"old.ldmpart", 100);
    EXPECT_TRUE(View(id).waitingForAddress);
    EXPECT_EQ(View(id).download.status, DownloadStatus::Paused);

    const ldm::model::Cookie cookie{"files.test", true, "/", true, 0, "sid", "new"};
    ldm::core::ReplaceAddressCommand replace;
    replace.id = id;
    replace.url = "https://files.test/new?token=2";
    replace.referer = "https://files.test/page";
    replace.userAgent = "BrowserUA/2.0";
    replace.cookies = {cookie};
    coordinator_->Handle(replace);
    ASSERT_EQ(engine_.starts.size(), 2u);
    const auto& resumed = engine_.starts.back();
    EXPECT_EQ(resumed.url, "https://files.test/new?token=2");
    EXPECT_EQ(resumed.userAgent, "BrowserUA/2.0");
    EXPECT_EQ(resumed.cookieLines, std::vector<std::string>{"files.test\tFALSE\t/\tTRUE\t0\tsid\tnew"});
    EXPECT_EQ(resumed.knownSize, 100) << "the old size still guards against a different file";
    EXPECT_EQ(resumed.segments.size(), 1u) << "progress is kept";
    EXPECT_FALSE(View(id).waitingForAddress);

    coordinator_->Handle(replace);
    EXPECT_EQ(engine_.starts.size(), 2u) << "a download that no longer waits ignores another address";
}

TEST_F(CoordinatorTest, AddressArrivingWhileTheOldRunStopsResumesAfterIt)
{
    const DownloadId id = *Add("https://files.test/a.zip").id;
    coordinator_->Handle(ldm::core::WaitForAddressCommand{id});
    ldm::core::ReplaceAddressCommand replace;
    replace.id = id;
    replace.url = "https://files.test/b.zip";
    coordinator_->Handle(replace);
    EXPECT_EQ(engine_.starts.size(), 1u) << "the old run has not ended yet";
    Finish(id, 1, TransferOutcome::Stopped, {{0, 10, 5}}, std::nullopt, 10);
    ASSERT_EQ(engine_.starts.size(), 2u);
    EXPECT_EQ(engine_.starts.back().url, "https://files.test/b.zip");
}

TEST_F(CoordinatorTest, OnlyOneDownloadWaitsAndWaitingEnds)
{
    const DownloadId first = *Add("https://files.test/1.zip", AddMode::Later).id;
    const DownloadId second = *Add("https://files.test/2.zip", AddMode::Later).id;
    coordinator_->Handle(ldm::core::WaitForAddressCommand{first});
    coordinator_->Handle(ldm::core::WaitForAddressCommand{second});
    EXPECT_FALSE(View(first).waitingForAddress) << "the newer request takes over";
    EXPECT_TRUE(View(second).waitingForAddress);

    coordinator_->Handle(ldm::core::ResumeCommand{{second}});
    EXPECT_FALSE(View(second).waitingForAddress) << "resuming by hand ends waiting";

    coordinator_->Handle(ldm::core::WaitForAddressCommand{first});
    clock_.Advance(ldm::core::kAddressWaitLimit);
    coordinator_->Tick();
    EXPECT_FALSE(View(first).waitingForAddress) << "waiting expires";

    coordinator_->Handle(ldm::core::WaitForAddressCommand{first});
    coordinator_->Handle(ldm::core::DeleteCommand{{first}, false});
    ldm::core::ReplaceAddressCommand replace;
    replace.id = first;
    replace.url = "https://files.test/1b.zip";
    coordinator_->Handle(replace);  // must not touch a deleted download
    EXPECT_FALSE(InSnapshot(first));
}

// Manifest fetches for the media panel go to the engine with the browser's cookies, and their
// answers go to the front end as they are.
TEST_F(CoordinatorTest, ManifestFetchesRunOnTheEngineAndAreAnswered)
{
    ldm::core::FetchManifestCommand command;
    command.fetchId = 3;
    command.url = "https://cdn.test/master.m3u8";
    command.referer = "https://site.test/watch";
    command.cookies = {{"cdn.test", true, "/", true, 0, "s", "1"}};
    command.maxBytes = 1024;
    coordinator_->Handle(command);
    ASSERT_EQ(engine_.fetches.size(), 1u);
    const auto& request = engine_.fetches[0];
    EXPECT_EQ(request.fetchId, 3);
    EXPECT_EQ(request.url, "https://cdn.test/master.m3u8");
    EXPECT_EQ(request.referer, "https://site.test/watch");
    EXPECT_EQ(request.userAgent, ldm::model::Settings{}.userAgent) << "Options' User-Agent when the browser sent none";
    EXPECT_EQ(request.maxBytes, 1024u);
    ASSERT_EQ(request.cookieLines.size(), 1u);

    ldm::core::FetchFinished finished;
    finished.fetchId = 3;
    finished.body = "#EXTM3U";
    Send(DownloadId{}, 0, finished);
    const auto answers = observer_.All<ldm::core::FetchFinished>();
    ASSERT_EQ(answers.size(), 1u);
    EXPECT_EQ(answers[0].body, "#EXTM3U");
}

TEST_F(CoordinatorTest, ManifestFetchesWaitForTheProxyLikeDownloads)
{
    proxy_.pending = true;
    ldm::core::FetchManifestCommand command;
    command.fetchId = 4;
    command.url = "https://cdn.test/a.mpd";
    coordinator_->Handle(command);
    EXPECT_TRUE(engine_.fetches.empty());
    ASSERT_EQ(proxy_.lookups.size(), 1u);
    EXPECT_LT(proxy_.lookups[0].first.value, 0) << "fetches use keys no download has";

    Send(proxy_.lookups[0].first, 0, ldm::core::ProxyResolved{"http://proxy.test:3128"});
    ASSERT_EQ(engine_.fetches.size(), 1u);
    EXPECT_EQ(engine_.fetches[0].proxy, "http://proxy.test:3128");
    Send(proxy_.lookups[0].first, 0, ldm::core::ProxyResolved{"http://again.test:1"});
    EXPECT_EQ(engine_.fetches.size(), 1u) << "a second answer starts nothing";
}

TEST_F(CoordinatorTest, ManifestFetchesRefuseOtherSchemes)
{
    ldm::core::FetchManifestCommand command;
    command.fetchId = 5;
    command.url = "ftp://cdn.test/a.m3u8";
    coordinator_->Handle(command);
    EXPECT_TRUE(engine_.fetches.empty());
    const auto answers = observer_.All<ldm::core::FetchFinished>();
    ASSERT_EQ(answers.size(), 1u);
    EXPECT_TRUE(answers[0].failure);
}

// ---------------------------------------------------------------------------------------------
// Download queues (M4 design section 3)

class QueueTest : public CoordinatorTest {
protected:
    const ldm::core::QueueView& QueueOf(ldm::model::QueueId id)
    {
        Publish();
        for (const auto& view : observer_.snapshot->queues) {
            if (view.queue.id == id) {
                return view;
            }
        }
        ADD_FAILURE() << "queue " << id.value << " not in the snapshot";
        static const ldm::core::QueueView empty;
        return empty;
    }

    ldm::model::QueueId MainQueue()
    {
        Publish();
        return observer_.snapshot->queues.at(0).queue.id;
    }

    ldm::model::QueueId SyncQueue()
    {
        Publish();
        return observer_.snapshot->queues.at(1).queue.id;
    }

    ldm::model::QueueId NewQueue(const std::string& name, int concurrent, int retries = 3,
                                 ldm::model::CompletionAction action = ldm::model::CompletionAction::None)
    {
        ldm::model::Queue queue;
        queue.name = name;
        queue.concurrent = concurrent;
        queue.retriesPerFile = retries;
        queue.completionAction = action;
        coordinator_->Handle(ldm::core::SaveQueueCommand{queue});
        Publish();
        return observer_.snapshot->queues.back().queue.id;
    }

    DownloadId AddTo(ldm::model::QueueId queue, const std::string& url)
    {
        return *Add(url, AddMode::Later, [queue](AddDownloadCommand& c) { c.queue = queue; }).id;
    }

    void Complete(DownloadId id, std::int64_t epoch)
    {
        Finish(id, epoch, TransferOutcome::Completed, {{0, 4, 4}}, std::nullopt, 4);
        Send(id, epoch, ldm::core::FinalizeFinished{});
    }

    std::vector<DownloadId> Started() const
    {
        std::vector<DownloadId> ids;
        for (const auto& start : engine_.starts) {
            ids.push_back(start.id);
        }
        return ids;
    }

    const TransferFailure notFound{FailureReason::NotFound, FailureClass::Fatal, 404, "HTTP status 404"};
};

TEST_F(QueueTest, DownloadLaterJoinsTheMainQueueAtTheEnd)
{
    const DownloadId a = *Add("http://x.test/a.dat", AddMode::Later).id;
    const DownloadId b = *Add("http://x.test/b.dat", AddMode::Later).id;
    const DownloadId now = *Add("http://x.test/c.dat").id;
    EXPECT_EQ(View(a).download.queueId, MainQueue());
    EXPECT_EQ(View(a).download.queuePosition, 1);
    EXPECT_EQ(View(b).download.queuePosition, 2);
    EXPECT_FALSE(View(now).download.queueId) << "a download started at once joins no queue";
    EXPECT_EQ(Stored(b)->queueId, MainQueue());
    EXPECT_EQ(QueueOf(MainQueue()).members, 2);
    EXPECT_FALSE(QueueOf(MainQueue()).running);
}

TEST_F(QueueTest, AddingToAQueueThatIsGoneFails)
{
    const auto result = Add("http://x.test/a.dat", AddMode::Later, [](AddDownloadCommand& c) { c.queue = {77}; });
    EXPECT_FALSE(result.id);
    EXPECT_TRUE(result.error);
}

TEST_F(QueueTest, DownloadLaterInTheInfoDialogJoinsTheMainQueue)
{
    const DownloadId id = *Add("http://x.test/a.iso", AddMode::Speculative).id;
    EXPECT_FALSE(View(id).download.queueId);
    ldm::core::ConfirmDownloadCommand confirm;
    confirm.id = id;
    confirm.fileName = "a.iso";
    confirm.saveDir = View(id).download.saveDir;
    confirm.start = false;
    coordinator_->Handle(confirm);
    EXPECT_EQ(View(id).download.queueId, MainQueue());
    EXPECT_EQ(Stored(id)->queueId, MainQueue());
}

TEST_F(QueueTest, AQueueStartsItsMembersInOrderAndFinishesWithItsAction)
{
    const auto queue = NewQueue("Nightly", 2, 3, ldm::model::CompletionAction::Shutdown);
    const DownloadId a = AddTo(queue, "http://x.test/a.dat");
    const DownloadId b = AddTo(queue, "http://x.test/b.dat");
    const DownloadId c = AddTo(queue, "http://x.test/c.dat");
    EXPECT_TRUE(engine_.starts.empty()) << "Download Later starts nothing";

    coordinator_->Handle(ldm::core::StartQueueCommand{queue});
    EXPECT_EQ(Started(), (std::vector<DownloadId>{a, b}));
    ASSERT_EQ(observer_.All<ldm::core::QueueStarted>().size(), 1u);
    EXPECT_EQ(observer_.All<ldm::core::QueueStarted>()[0].name, "Nightly");
    EXPECT_TRUE(View(c).waitingInQueue);
    EXPECT_FALSE(View(a).waitingInQueue);
    EXPECT_TRUE(QueueOf(queue).running);
    EXPECT_EQ(QueueOf(queue).members, 3);
    EXPECT_EQ(QueueOf(queue).runningMembers, 2);

    Complete(a, 1);
    EXPECT_FALSE(View(a).download.queueId) << "a completed member leaves its queue";
    EXPECT_EQ(Started(), (std::vector<DownloadId>{a, b, c})) << "the free slot is filled on the next tick";
    EXPECT_FALSE(View(c).waitingInQueue);

    Complete(b, 1);
    Complete(c, 1);
    Publish();
    const auto finished = observer_.All<ldm::core::QueueFinished>();
    ASSERT_EQ(finished.size(), 1u);
    EXPECT_EQ(finished[0].id, queue);
    EXPECT_EQ(finished[0].completionAction, ldm::model::CompletionAction::Shutdown);
    EXPECT_FALSE(QueueOf(queue).running);
    EXPECT_EQ(QueueOf(queue).members, 0);
    EXPECT_TRUE(observer_.All<ldm::core::QueueStopped>().empty());
}

TEST_F(QueueTest, AQueueThatStartsNothingFinishesWithoutItsAction)
{
    const auto queue = NewQueue("Empty", 1, 3, ldm::model::CompletionAction::Shutdown);
    coordinator_->Handle(ldm::core::StartQueueCommand{queue});
    const auto finished = observer_.All<ldm::core::QueueFinished>();
    ASSERT_EQ(finished.size(), 1u);
    EXPECT_EQ(finished[0].completionAction, ldm::model::CompletionAction::None) << "an empty queue never shuts down";
    EXPECT_FALSE(QueueOf(queue).running);
}

TEST_F(QueueTest, AQueueWhoseMembersTheUserStoppedFinishesWithoutItsAction)
{
    const auto queue = NewQueue("Nightly", 2, 3, ldm::model::CompletionAction::Shutdown);
    const DownloadId a = AddTo(queue, "http://x.test/a.dat");
    const DownloadId b = AddTo(queue, "http://x.test/b.dat");
    coordinator_->Handle(ldm::core::StartQueueCommand{queue});
    coordinator_->Handle(ldm::core::StopCommand{{a, b}});
    Finish(a, 1, TransferOutcome::Stopped, {{0, 100, 30}}, std::nullopt, 100);
    Finish(b, 1, TransferOutcome::Stopped, {{0, 100, 30}}, std::nullopt, 100);
    Publish();
    const auto finished = observer_.All<ldm::core::QueueFinished>();
    ASSERT_EQ(finished.size(), 1u);
    EXPECT_EQ(finished[0].completionAction, ldm::model::CompletionAction::None) << "nothing was completed";
}

TEST_F(QueueTest, AQueueWithAMemberTheUserStoppedFinishesWithoutItsAction)
{
    const auto queue = NewQueue("Nightly", 2, 3, ldm::model::CompletionAction::Shutdown);
    const DownloadId a = AddTo(queue, "http://x.test/a.dat");
    const DownloadId b = AddTo(queue, "http://x.test/b.dat");
    coordinator_->Handle(ldm::core::StartQueueCommand{queue});
    coordinator_->Handle(ldm::core::StopCommand{{b}});
    Finish(b, 1, TransferOutcome::Stopped, {{0, 100, 30}}, std::nullopt, 100);
    Complete(a, 1);
    Publish();
    const auto finished = observer_.All<ldm::core::QueueFinished>();
    ASSERT_EQ(finished.size(), 1u);
    EXPECT_EQ(finished[0].completionAction, ldm::model::CompletionAction::None)
        << "the user held back work: the computer stays on";
}

TEST_F(QueueTest, StartingARunningQueueAgainChangesNothing)
{
    const auto queue = NewQueue("Nightly", 1);
    AddTo(queue, "http://x.test/a.dat");
    AddTo(queue, "http://x.test/b.dat");
    coordinator_->Handle(ldm::core::StartQueueCommand{queue});
    coordinator_->Handle(ldm::core::StartQueueCommand{queue});
    EXPECT_EQ(engine_.starts.size(), 1u);
    EXPECT_EQ(observer_.All<ldm::core::QueueStarted>().size(), 1u);
}

TEST_F(QueueTest, AFailedMemberIsTriedAgainAfterTheOthersUntilItsRetriesAreUsed)
{
    const auto queue = NewQueue("Retry", 1, 1);
    const DownloadId a = AddTo(queue, "http://x.test/a.dat");
    const DownloadId b = AddTo(queue, "http://x.test/b.dat");
    coordinator_->Handle(ldm::core::StartQueueCommand{queue});
    ASSERT_EQ(Started(), (std::vector<DownloadId>{a}));

    Finish(a, 1, TransferOutcome::Failed, {}, notFound);
    EXPECT_EQ(View(a).download.queueFailures, 1);
    EXPECT_GT(View(a).download.queuePosition, View(b).download.queuePosition) << "moved behind the others";
    EXPECT_EQ(Started(), (std::vector<DownloadId>{a, b}));

    Complete(b, 1);
    Publish();
    EXPECT_EQ(Started(), (std::vector<DownloadId>{a, b, a})) << "one retry";
    Finish(a, 2, TransferOutcome::Failed, {}, notFound);
    Publish();
    EXPECT_EQ(engine_.starts.size(), 3u) << "no retries left";
    EXPECT_EQ(View(a).download.status, DownloadStatus::Failed);
    EXPECT_EQ(View(a).download.queueId, queue) << "a failed member stays in its queue";
    ASSERT_EQ(observer_.All<ldm::core::QueueFinished>().size(), 1u);

    // The next run gives it its retries again.
    coordinator_->Handle(ldm::core::StartQueueCommand{queue});
    EXPECT_EQ(View(a).download.queueFailures, 0);
    EXPECT_EQ(engine_.starts.size(), 4u);
}

TEST_F(QueueTest, AMemberStoppedByHandIsLeftAloneForTheRestOfTheRun)
{
    const auto queue = NewQueue("Nightly", 1);
    const DownloadId a = AddTo(queue, "http://x.test/a.dat");
    const DownloadId b = AddTo(queue, "http://x.test/b.dat");
    coordinator_->Handle(ldm::core::StartQueueCommand{queue});
    coordinator_->Handle(ldm::core::StopCommand{{a}});
    Finish(a, 1, TransferOutcome::Stopped, {{0, 100, 30}}, std::nullopt, 100);
    Publish();
    EXPECT_EQ(Started(), (std::vector<DownloadId>{a, b}));
    EXPECT_FALSE(View(a).waitingInQueue);
    EXPECT_EQ(View(a).download.queueId, queue) << "stopping keeps the membership";

    Complete(b, 1);
    Publish();
    EXPECT_EQ(engine_.starts.size(), 2u);
    EXPECT_EQ(observer_.All<ldm::core::QueueFinished>().size(), 1u);
}

TEST_F(QueueTest, ResumingAMemberByHandStartsItBesideTheQueue)
{
    const auto queue = NewQueue("Nightly", 1);
    const DownloadId a = AddTo(queue, "http://x.test/a.dat");
    const DownloadId b = AddTo(queue, "http://x.test/b.dat");
    coordinator_->Handle(ldm::core::StartQueueCommand{queue});
    coordinator_->Handle(ldm::core::ResumeCommand{{b}});
    EXPECT_EQ(Started(), (std::vector<DownloadId>{a, b})) << "a manual start ignores the queue's concurrency";
    EXPECT_EQ(QueueOf(queue).runningMembers, 2);
}

TEST_F(QueueTest, StoppingAQueueStopsItsRunningMembersWithoutItsAction)
{
    const auto queue = NewQueue("Nightly", 2, 3, ldm::model::CompletionAction::Shutdown);
    const DownloadId a = AddTo(queue, "http://x.test/a.dat");
    const DownloadId b = AddTo(queue, "http://x.test/b.dat");
    AddTo(queue, "http://x.test/c.dat");
    coordinator_->Handle(ldm::core::StartQueueCommand{queue});
    coordinator_->Handle(ldm::core::StopQueueCommand{queue});
    EXPECT_EQ(engine_.stops.size(), 2u);
    ASSERT_EQ(observer_.All<ldm::core::QueueStopped>().size(), 1u);
    Finish(a, 1, TransferOutcome::Stopped, {{0, 100, 30}}, std::nullopt, 100);
    Finish(b, 1, TransferOutcome::Stopped, {{0, 100, 30}}, std::nullopt, 100);
    Publish();
    EXPECT_EQ(engine_.starts.size(), 2u) << "a stopped queue starts nothing";
    EXPECT_TRUE(observer_.All<ldm::core::QueueFinished>().empty());
    EXPECT_FALSE(QueueOf(queue).running);
}

TEST_F(QueueTest, StopAllStopsEveryQueue)
{
    const auto queue = NewQueue("Nightly", 1);
    AddTo(queue, "http://x.test/a.dat");
    coordinator_->Handle(ldm::core::StartQueueCommand{queue});
    coordinator_->Handle(ldm::core::StopAllCommand{});
    EXPECT_EQ(observer_.All<ldm::core::QueueStopped>().size(), 1u);
    EXPECT_FALSE(QueueOf(queue).running);
}

TEST_F(QueueTest, ACompletedDownloadAddedToAQueueIsDownloadedAgainWhenItsTurnComes)
{
    const DownloadId id = *Add("http://x.test/a.dat").id;
    Complete(id, 1);
    files_.Put(kRoot / L"a.dat", 4);
    coordinator_->Handle(ldm::core::SetQueueCommand{{id}, MainQueue()});
    EXPECT_EQ(View(id).download.status, DownloadStatus::Completed);

    coordinator_->Handle(ldm::core::StartQueueCommand{MainQueue()});
    ASSERT_EQ(engine_.starts.size(), 2u);
    EXPECT_TRUE(engine_.starts.back().segments.empty()) << "from the start";
    Finish(id, 2, TransferOutcome::Completed, {{0, 4, 4}}, std::nullopt, 4);
    EXPECT_FALSE(files_.Exists(kRoot / L"a.dat")) << "the old file goes only once the new one is complete";
    Send(id, 2, ldm::core::FinalizeFinished{});
    Publish();
    EXPECT_FALSE(View(id).download.queueId);
    EXPECT_EQ(observer_.All<ldm::core::QueueFinished>().size(), 1u);
}

TEST_F(QueueTest, TheSynchronizationQueueKeepsItsCompletedMembers)
{
    const DownloadId id = AddTo(SyncQueue(), "http://x.test/a.dat");
    coordinator_->Handle(ldm::core::StartQueueCommand{SyncQueue()});
    ASSERT_EQ(engine_.starts.size(), 1u);
    Complete(id, 1);
    Publish();
    EXPECT_EQ(View(id).download.queueId, SyncQueue());
    EXPECT_EQ(engine_.starts.size(), 1u) << "a completed member is not downloaded again";
    EXPECT_EQ(observer_.All<ldm::core::QueueFinished>().size(), 1u);
}

TEST_F(QueueTest, QueuesAreStoppedAfterARestartAndKeepTheirMembers)
{
    const auto queue = NewQueue("Nightly", 1);
    const DownloadId a = AddTo(queue, "http://x.test/a.dat");
    const DownloadId b = AddTo(queue, "http://x.test/b.dat");
    coordinator_->Handle(ldm::core::StartQueueCommand{queue});
    Recreate();
    EXPECT_FALSE(QueueOf(queue).running);
    EXPECT_EQ(QueueOf(queue).members, 2);
    EXPECT_EQ(View(a).download.status, DownloadStatus::Paused);
    EXPECT_EQ(View(b).download.queuePosition, 2);
    EXPECT_FALSE(View(b).waitingInQueue);
}

TEST_F(QueueTest, ACompletedMemberFoundAfterACrashLeavesItsQueue)
{
    const DownloadId id = *Add("http://x.test/a.dat").id;
    coordinator_->Handle(ldm::core::SetQueueCommand{{id}, MainQueue()});
    Finish(id, 1, TransferOutcome::Completed, {{0, 4, 4}}, std::nullopt, 4);  // the move never answers
    files_.Put(kRoot / L"a.dat", 4);
    Recreate();
    EXPECT_EQ(View(id).download.status, DownloadStatus::Completed);
    EXPECT_FALSE(View(id).download.queueId) << "otherwise the next run would download it again";
}

TEST_F(QueueTest, ReorderingDecidesWhichMemberStartsFirst)
{
    const auto queue = NewQueue("Nightly", 1);
    const DownloadId a = AddTo(queue, "http://x.test/a.dat");
    const DownloadId b = AddTo(queue, "http://x.test/b.dat");
    const DownloadId c = AddTo(queue, "http://x.test/c.dat");
    coordinator_->Handle(ldm::core::ReorderQueueCommand{queue, {c, a, DownloadId{999}, c}});
    EXPECT_EQ(View(c).download.queuePosition, 1);
    EXPECT_EQ(View(a).download.queuePosition, 2);
    EXPECT_EQ(View(b).download.queuePosition, 3) << "members left out follow those listed";
    EXPECT_EQ(Stored(c)->queuePosition, 1);
    coordinator_->Handle(ldm::core::StartQueueCommand{queue});
    EXPECT_EQ(Started(), (std::vector<DownloadId>{c}));
}

TEST_F(QueueTest, MovingDownloadsToAQueueKeepsTheListOrder)
{
    const auto queue = NewQueue("Nightly", 1);
    const DownloadId a = *Add("http://x.test/a.dat", AddMode::Later).id;
    const DownloadId b = *Add("http://x.test/b.dat", AddMode::Later).id;
    coordinator_->Handle(ldm::core::SetQueueCommand{{b, a}, queue});
    EXPECT_EQ(View(a).download.queueId, queue);
    EXPECT_EQ(View(a).download.queuePosition, 1);
    EXPECT_EQ(View(b).download.queuePosition, 2);
    EXPECT_EQ(QueueOf(MainQueue()).members, 0);

    coordinator_->Handle(ldm::core::SetQueueCommand{{a}, std::nullopt});
    EXPECT_FALSE(View(a).download.queueId);
    EXPECT_FALSE(Stored(a)->queueId);
}

TEST_F(QueueTest, ANewQueueCanTakeMembersAtOnce)
{
    const DownloadId a = *Add("http://x.test/a.dat", AddMode::Later).id;
    ldm::model::Queue queue;
    queue.name = "Fresh";
    coordinator_->Handle(ldm::core::SaveQueueCommand{queue, {a}});
    Publish();
    const auto fresh = observer_.snapshot->queues.back().queue.id;
    EXPECT_EQ(View(a).download.queueId, fresh);
    EXPECT_EQ(QueueOf(fresh).members, 1);
}

TEST_F(QueueTest, SavingQueuesChecksWhatTheyAreGiven)
{
    const auto nightly = NewQueue("  Nightly ", 2);
    EXPECT_EQ(QueueOf(nightly).queue.name, "Nightly");
    EXPECT_EQ(QueueOf(nightly).queue.kind, ldm::model::QueueKind::Custom);

    const auto errorsBefore = observer_.All<ldm::core::CoreError>().size();
    ldm::model::Queue duplicate;
    duplicate.name = "NIGHTLY";
    coordinator_->Handle(ldm::core::SaveQueueCommand{duplicate});
    ldm::model::Queue wide;
    wide.name = "Wide";
    wide.concurrent = ldm::model::kMaxQueueConcurrent + 1;
    coordinator_->Handle(ldm::core::SaveQueueCommand{wide});
    ldm::model::Queue noDays;
    noDays.name = "No days";
    noDays.schedule.enabled = true;
    noDays.schedule.weekdays = 0;
    coordinator_->Handle(ldm::core::SaveQueueCommand{noDays});
    ldm::model::Queue relative;
    relative.name = "Relative";
    relative.openFileWhenDone = "done.cmd";
    coordinator_->Handle(ldm::core::SaveQueueCommand{relative});
    EXPECT_EQ(observer_.All<ldm::core::CoreError>().size(), errorsBefore + 4);
    EXPECT_EQ(observer_.snapshot->queues.size(), 3u);

    ldm::model::Queue main = QueueOf(MainQueue()).queue;
    main.name = "Renamed";
    main.concurrent = 4;
    coordinator_->Handle(ldm::core::SaveQueueCommand{main});
    EXPECT_EQ(QueueOf(MainQueue()).queue.name, "Main download queue") << "the built-in queues keep their names";
    EXPECT_EQ(QueueOf(MainQueue()).queue.concurrent, 4);
    EXPECT_EQ(QueueOf(MainQueue()).queue.kind, ldm::model::QueueKind::Main);
}

TEST_F(QueueTest, DeletingAQueueLeavesItsMembersOutsideAnyQueue)
{
    const auto queue = NewQueue("Nightly", 1);
    const DownloadId a = AddTo(queue, "http://x.test/a.dat");
    coordinator_->Handle(ldm::core::StartQueueCommand{queue});
    coordinator_->Handle(ldm::core::DeleteQueueCommand{queue});
    EXPECT_EQ(observer_.All<ldm::core::QueueStopped>().size(), 1u) << "a running queue stops first";
    EXPECT_FALSE(View(a).download.queueId);
    EXPECT_FALSE(Stored(a)->queueId);
    EXPECT_EQ(observer_.snapshot->queues.size(), 2u);

    const auto errorsBefore = observer_.All<ldm::core::CoreError>().size();
    coordinator_->Handle(ldm::core::DeleteQueueCommand{MainQueue()});
    EXPECT_EQ(observer_.All<ldm::core::CoreError>().size(), errorsBefore + 1);
    EXPECT_EQ(observer_.snapshot->queues.size(), 2u);
}

TEST_F(QueueTest, AQueueWaitsForTheQuotaInsteadOfFinishing)
{
    EnableQuota(*coordinator_, false);
    const DownloadId first = *Add("http://x.test/first.dat").id;
    Send(first, 1, Received(1100 * 1024));
    Finish(first, 1, TransferOutcome::Stopped, {{0, std::nullopt, 1100 * 1024}});

    const auto queue = NewQueue("Nightly", 1);
    const DownloadId a = AddTo(queue, "http://x.test/a.dat");
    coordinator_->Handle(ldm::core::StartQueueCommand{queue});
    Publish();
    EXPECT_EQ(engine_.starts.size(), 1u) << "nothing starts while the quota is used up";
    EXPECT_TRUE(QueueOf(queue).running);
    EXPECT_TRUE(observer_.All<ldm::core::QueueFinished>().empty());

    clock_.Advance(std::chrono::hours(1));
    Publish();
    const auto started = Started();
    EXPECT_NE(std::find(started.begin(), started.end(), a), started.end()) << "it starts when the period ends";
}


// ---------------------------------------------------------------------------------------------
// Synchronization queue (M4 design section 5)

class SyncTest : public QueueTest {
protected:
    // A completed download of 4 bytes with an ETag, now a member of the synchronization queue.
    DownloadId CompletedMember()
    {
        const DownloadId id = *Add("http://x.test/a.dat").id;
        ResponseInfo info;
        info.finalUrl = "http://x.test/a.dat";
        info.totalSize = 4;
        info.resumable = true;
        info.validators.etag = "\"v1\"";
        Send(id, 1, info);
        Complete(id, 1);
        files_.Put(kRoot / L"a.dat", 4);
        coordinator_->Handle(ldm::core::SetQueueCommand{{id}, SyncQueue()});
        return id;
    }
};

TEST_F(SyncTest, AnUnchangedFileIsLeftAlone)
{
    const DownloadId id = CompletedMember();
    coordinator_->Handle(ldm::core::StartQueueCommand{SyncQueue()});
    ASSERT_EQ(engine_.starts.size(), 2u);
    const auto& check = engine_.starts.back();
    EXPECT_TRUE(check.skipIfUnchanged);
    EXPECT_EQ(check.validators.etag, "\"v1\"");
    EXPECT_EQ(check.knownSize, 4);
    EXPECT_TRUE(check.segments.empty());
    EXPECT_NE(check.partialPath, kRoot / L"a.dat");
    files_.Put(check.partialPath, 0);

    Finish(id, 2, TransferOutcome::Unchanged);
    Publish();
    const auto& view = View(id);
    EXPECT_EQ(view.download.status, DownloadStatus::Completed);
    EXPECT_EQ(view.download.lastResult, "Unchanged on the server");
    EXPECT_EQ(view.download.DownloadedBytes(), 4);
    EXPECT_EQ(view.download.validators.etag, "\"v1\"");
    EXPECT_FALSE(view.download.partialPath);
    EXPECT_FALSE(files_.Exists(check.partialPath)) << "the empty partial file goes";
    EXPECT_TRUE(files_.Exists(kRoot / L"a.dat"));
    EXPECT_EQ(engine_.finalizes.size(), 1u);
    EXPECT_EQ(observer_.All<ldm::core::DownloadCompleted>().size(), 1u);
    EXPECT_EQ(view.download.queueId, SyncQueue());
    EXPECT_EQ(observer_.All<ldm::core::QueueFinished>().size(), 1u) << "checked once per run";
    EXPECT_EQ(Stored(id)->status, DownloadStatus::Completed);

    coordinator_->Handle(ldm::core::StartQueueCommand{SyncQueue()});
    EXPECT_EQ(engine_.starts.size(), 3u) << "and again on the next run";
}

TEST_F(SyncTest, AChangedFileReplacesTheOldOneAndStaysInTheQueue)
{
    const DownloadId id = CompletedMember();
    coordinator_->Handle(ldm::core::StartQueueCommand{SyncQueue()});
    Finish(id, 2, TransferOutcome::Completed, {{0, 6, 6}}, std::nullopt, 6);
    EXPECT_FALSE(files_.Exists(kRoot / L"a.dat")) << "replaced once the new copy is complete";
    EXPECT_EQ(engine_.finalizes.back().finalPath, kRoot / L"a.dat");
    Send(id, 2, ldm::core::FinalizeFinished{});
    Publish();
    EXPECT_EQ(View(id).download.status, DownloadStatus::Completed);
    EXPECT_EQ(View(id).download.totalSize, 6);
    EXPECT_EQ(View(id).download.queueId, SyncQueue());
    EXPECT_EQ(observer_.All<ldm::core::DownloadCompleted>().size(), 2u);
    EXPECT_EQ(engine_.starts.size(), 2u);
    EXPECT_EQ(observer_.All<ldm::core::QueueFinished>().size(), 1u);
}

TEST_F(SyncTest, ACheckThatEndsBeforeAnythingArrivedKeepsTheCompletedDownload)
{
    const DownloadId id = CompletedMember();
    coordinator_->Handle(ldm::core::StartQueueCommand{SyncQueue()});
    Finish(id, 2, TransferOutcome::Failed, {}, notFound);
    EXPECT_EQ(View(id).download.status, DownloadStatus::Completed);
    EXPECT_EQ(View(id).download.DownloadedBytes(), 4);
    ASSERT_TRUE(View(id).download.lastResult);
    EXPECT_TRUE(View(id).download.lastResult->starts_with("Could not check the server for changes"));
    EXPECT_TRUE(observer_.All<ldm::core::DownloadFailed>().empty()) << "a failed check is no failed download";

    coordinator_->Handle(ldm::core::StartQueueCommand{SyncQueue()});
    coordinator_->Handle(ldm::core::StopQueueCommand{SyncQueue()});
    Finish(id, 3, TransferOutcome::Stopped);
    EXPECT_EQ(View(id).download.status, DownloadStatus::Completed);
    EXPECT_EQ(View(id).download.lastResult, "The check for changes was stopped");
}

TEST_F(SyncTest, ACheckInProgressIsStoredAsTheCompletedDownload)
{
    const DownloadId id = CompletedMember();
    coordinator_->Handle(ldm::core::StartQueueCommand{SyncQueue()});
    ASSERT_EQ(engine_.starts.size(), 2u);
    const auto stored = Stored(id);
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->status, DownloadStatus::Completed) << "a crash now leaves the download complete";
    EXPECT_EQ(stored->DownloadedBytes(), 4);
    EXPECT_FALSE(stored->partialPath);

    coordinator_.reset();
    Recreate();
    Publish();
    EXPECT_EQ(View(id).download.status, DownloadStatus::Completed);
    EXPECT_EQ(View(id).download.totalSize, 4);
}

TEST_F(SyncTest, ANewFileFoundByACheckReplacesTheOldOneAfterARestart)
{
    const DownloadId id = CompletedMember();
    coordinator_->Handle(ldm::core::StartQueueCommand{SyncQueue()});
    coordinator_->Handle(ldm::core::StopQueueCommand{SyncQueue()});
    Finish(id, 2, TransferOutcome::Stopped, {{0, 6, 3}}, std::nullopt, 6);
    ASSERT_EQ(Stored(id)->status, DownloadStatus::Paused);
    EXPECT_TRUE(Stored(id)->replaceFinalFile) << "stored, so a restart still replaces the old file";

    coordinator_.reset();
    Recreate();
    coordinator_->Handle(ldm::core::ResumeCommand{{id}});
    const std::int64_t epoch = engine_.starts.back().epoch;
    Finish(id, epoch, TransferOutcome::Completed, {{0, 6, 6}}, std::nullopt, 6);
    ASSERT_FALSE(engine_.finalizes.empty());
    EXPECT_EQ(engine_.finalizes.back().finalPath, kRoot / L"a.dat") << "no numbered copy beside the old file";
}

TEST_F(SyncTest, AMemberMovedToAnotherQueueDuringACheckLeavesIt)
{
    const DownloadId id = CompletedMember();
    const auto other = NewQueue("Other", 1);
    coordinator_->Handle(ldm::core::StartQueueCommand{SyncQueue()});
    coordinator_->Handle(ldm::core::SetQueueCommand{{id}, other});
    Finish(id, 2, TransferOutcome::Unchanged);
    Publish();
    EXPECT_EQ(View(id).download.status, DownloadStatus::Completed);
    EXPECT_FALSE(View(id).download.queueId) << "a completed download leaves an ordinary queue";
    coordinator_->Handle(ldm::core::StartQueueCommand{other});
    EXPECT_EQ(engine_.starts.size(), 2u) << "and is not downloaded again";
}

TEST_F(SyncTest, ACheckThatCannotBeginIsNoFailure)
{
    const DownloadId id = CompletedMember();
    files_.failDirectories = true;
    coordinator_->Handle(ldm::core::StartQueueCommand{SyncQueue()});
    Publish();
    EXPECT_EQ(engine_.starts.size(), 1u);
    EXPECT_EQ(View(id).download.status, DownloadStatus::Completed);
    ASSERT_TRUE(View(id).download.lastResult);
    EXPECT_TRUE(View(id).download.lastResult->starts_with("Could not check the server for changes"));
    EXPECT_TRUE(observer_.All<ldm::core::DownloadFailed>().empty());
    EXPECT_EQ(View(id).download.queueId, SyncQueue());
}

TEST_F(SyncTest, ACheckStoppedWithPartOfANewFileContinuesLikeARedownload)
{
    const DownloadId id = CompletedMember();
    coordinator_->Handle(ldm::core::StartQueueCommand{SyncQueue()});
    coordinator_->Handle(ldm::core::StopQueueCommand{SyncQueue()});
    Finish(id, 2, TransferOutcome::Stopped, {{0, 6, 3}}, std::nullopt, 6);
    EXPECT_EQ(View(id).download.status, DownloadStatus::Paused);
    EXPECT_EQ(View(id).download.DownloadedBytes(), 3);
    EXPECT_TRUE(files_.Exists(kRoot / L"a.dat")) << "the old file stays until the new one is complete";

    coordinator_->Handle(ldm::core::ResumeCommand{{id}});
    EXPECT_FALSE(engine_.starts.back().skipIfUnchanged);
    EXPECT_EQ(engine_.starts.back().segments.size(), 1u);
}

// ---------------------------------------------------------------------------------------------
// Queue schedules (M4 design section 4). The fake clock starts on a Friday at 08:00 UTC.

constexpr std::int64_t kMinuteMs = 60'000;
constexpr std::int64_t kFridayMidnight = 1'800'000'000'000 - 8 * 60 * kMinuteMs;

std::int64_t FridayAt(int hours, int minutes)
{
    return kFridayMidnight + (std::int64_t{hours} * 60 + minutes) * kMinuteMs;
}

ldm::model::Schedule DailyAt(int hours, int minutes)
{
    ldm::model::Schedule schedule;
    schedule.enabled = true;
    schedule.mode = ldm::model::ScheduleMode::Daily;
    schedule.startMinutes = hours * 60 + minutes;
    return schedule;
}

class ScheduleTest : public QueueTest {
protected:
    void SetUp() override
    {
        QueueTest::SetUp();
        timeZone_ = &utc_;
        Recreate();
    }

    ldm::model::QueueId Scheduled(const ldm::model::Schedule& schedule, const std::string& name = "Nightly")
    {
        ldm::model::Queue queue;
        queue.name = name;
        queue.schedule = schedule;
        coordinator_->Handle(ldm::core::SaveQueueCommand{queue});
        Publish();
        const auto id = observer_.snapshot->queues.back().queue.id;
        AddTo(id, "http://x.test/" + name + ".dat");
        return id;
    }

    void Wait(std::chrono::minutes minutes)
    {
        clock_.Advance(minutes);
        coordinator_->Tick();
    }

    UtcZone utc_;
};

TEST_F(ScheduleTest, AQueueStartsAtItsScheduledTime)
{
    const auto queue = Scheduled(DailyAt(8, 30));
    EXPECT_EQ(wakeTimer_.armed, FridayAt(8, 30)) << "the computer may sleep until then";
    Wait(std::chrono::minutes(29));
    EXPECT_TRUE(engine_.starts.empty());
    EXPECT_LE(coordinator_->NextDeadline(), clock_.Steady() + std::chrono::minutes(1));

    Wait(std::chrono::minutes(1));
    EXPECT_EQ(engine_.starts.size(), 1u);
    EXPECT_EQ(observer_.All<ldm::core::QueueStarted>().size(), 1u);
    EXPECT_TRUE(QueueOf(queue).running);
    EXPECT_TRUE(QueueOf(queue).queue.schedule.lastStartedAtMs);
    EXPECT_EQ(wakeTimer_.armed, FridayAt(8, 30) + 24 * 60 * kMinuteMs) << "then the next day's start";
}

TEST_F(ScheduleTest, AQueueStopsAtItsStopTime)
{
    ldm::model::Schedule schedule = DailyAt(8, 30);
    schedule.stopEnabled = true;
    schedule.stopMinutes = 9 * 60;
    const auto queue = Scheduled(schedule);
    Wait(std::chrono::minutes(31));
    ASSERT_TRUE(QueueOf(queue).running);
    Wait(std::chrono::minutes(30));
    EXPECT_FALSE(QueueOf(queue).running);
    EXPECT_EQ(observer_.All<ldm::core::QueueStopped>().size(), 1u);
    EXPECT_EQ(engine_.stops.size(), 1u);
    EXPECT_TRUE(observer_.All<ldm::core::QueueFinished>().empty()) << "no completion action at the stop time";
}

TEST_F(ScheduleTest, AStopAtTheStartTimeEndsTheRunOnTheNextDay)
{
    ldm::model::Schedule schedule = DailyAt(8, 30);
    schedule.stopEnabled = true;
    schedule.stopMinutes = 8 * 60 + 30;
    const auto queue = Scheduled(schedule);
    Wait(std::chrono::minutes(31));
    EXPECT_TRUE(QueueOf(queue).running) << "the stop belongs to the previous day's run";
    EXPECT_EQ(observer_.All<ldm::core::QueueStarted>().size(), 1u);
}

TEST_F(ScheduleTest, AStopThatPassedAfterAStartWhileAsleepLeavesTheQueueStopped)
{
    ldm::model::Schedule schedule = DailyAt(8, 30);
    schedule.stopEnabled = true;
    schedule.stopMinutes = 8 * 60 + 35;
    const auto queue = Scheduled(schedule);
    clock_.Advance(std::chrono::minutes(40));  // asleep through both
    coordinator_->Handle(ldm::core::ClockChangedCommand{});
    EXPECT_FALSE(QueueOf(queue).running);
    EXPECT_TRUE(engine_.starts.empty());
}

TEST_F(ScheduleTest, AStartMissedWhileLdmWasClosedIsNotCaughtUp)
{
    const auto queue = Scheduled(DailyAt(8, 30));
    coordinator_.reset();
    clock_.Advance(std::chrono::minutes(40));
    Recreate();
    Publish();
    EXPECT_TRUE(engine_.starts.empty());
    EXPECT_FALSE(QueueOf(queue).running);
}

TEST_F(ScheduleTest, AStartTheComputerSleptThroughHappensWhenItWakesSoonAfter)
{
    const auto queue = Scheduled(DailyAt(8, 30));
    clock_.Advance(std::chrono::minutes(35));  // asleep: no ticks
    coordinator_->Handle(ldm::core::ClockChangedCommand{});
    EXPECT_TRUE(QueueOf(queue).running);
}

TEST_F(ScheduleTest, AStartLongBeforeTheComputerWokeIsMissed)
{
    const auto queue = Scheduled(DailyAt(8, 30));
    clock_.Advance(std::chrono::minutes(50));
    coordinator_->Handle(ldm::core::ClockChangedCommand{});
    coordinator_->Tick();
    EXPECT_FALSE(QueueOf(queue).running);
}

TEST_F(ScheduleTest, AQueueCanStartWithLdm)
{
    ldm::model::Schedule schedule;
    schedule.startOnAppStart = true;
    const auto queue = Scheduled(schedule);
    EXPECT_FALSE(QueueOf(queue).running);
    Recreate();
    EXPECT_TRUE(QueueOf(queue).running);
    EXPECT_EQ(engine_.starts.size(), 1u);
}

TEST_F(ScheduleTest, ASingleStartHappensOnceEvenWhenTheClockGoesBack)
{
    ldm::model::Schedule schedule;
    schedule.enabled = true;
    schedule.mode = ldm::model::ScheduleMode::Once;
    schedule.startDay = kFridayMidnight / (24 * 60 * kMinuteMs);
    schedule.startMinutes = 8 * 60 + 30;
    const auto queue = Scheduled(schedule);
    Wait(std::chrono::minutes(31));
    ASSERT_TRUE(QueueOf(queue).running);
    coordinator_->Handle(ldm::core::StopQueueCommand{queue});

    clock_.unixMs -= 10 * kMinuteMs;
    coordinator_->Handle(ldm::core::ClockChangedCommand{});
    Wait(std::chrono::minutes(10));
    EXPECT_FALSE(QueueOf(queue).running);
    EXPECT_EQ(observer_.All<ldm::core::QueueStarted>().size(), 1u);
    EXPECT_FALSE(wakeTimer_.armed) << "nothing left to wake up for";
}

TEST_F(ScheduleTest, TheWakeTimerIsSetForTheEarliestStart)
{
    const auto nine = Scheduled(DailyAt(9, 0), "Nine");
    const auto early = Scheduled(DailyAt(8, 45), "Early");
    EXPECT_EQ(wakeTimer_.armed, FridayAt(8, 45));

    ldm::model::Queue changed = QueueOf(early).queue;
    changed.schedule.enabled = false;
    coordinator_->Handle(ldm::core::SaveQueueCommand{changed});
    EXPECT_EQ(wakeTimer_.armed, FridayAt(9, 0));

    coordinator_->Handle(ldm::core::DeleteQueueCommand{nine});
    EXPECT_FALSE(wakeTimer_.armed);
}
