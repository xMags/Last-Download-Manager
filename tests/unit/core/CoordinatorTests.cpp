#include "base/Paths.h"
#include "base/Utf.h"
#include "core/Coordinator.h"
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
    void Stop(DownloadId id, std::int64_t epoch) override { stops.emplace_back(id, epoch); }
    void SetSpeedLimit(DownloadId, std::int64_t, std::optional<std::int64_t> limit) override { downloadLimit = limit; }
    void SetGlobalSpeedLimit(std::optional<std::int64_t> limit) override { globalLimit = limit; }
    void Finalize(ldm::core::FinalizeRequest request) override { finalizes.push_back(std::move(request)); }
    void Fetch(ldm::core::FetchRequest request) override { fetches.push_back(std::move(request)); }

    std::vector<ldm::core::TransferRequest> starts;
    std::vector<ldm::core::MediaRequest> mediaStarts;
    std::vector<std::pair<DownloadId, std::int64_t>> stops;
    std::vector<ldm::core::FinalizeRequest> finalizes;
    std::vector<ldm::core::FetchRequest> fetches;
    std::optional<std::int64_t> downloadLimit;
    std::optional<std::int64_t> globalLimit;
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
    ldm::base::Status CreateDirectories(const fs::path&) override { return {}; }

    void Put(const fs::path& path, std::int64_t size) { files[ldm::base::PathComparisonKey(path)] = size; }

    std::map<std::wstring, std::int64_t> files;
    std::vector<fs::path> removed;
    std::vector<fs::path> recycled;
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

TEST_F(CoordinatorTest, InconsistentServerRestartsOnceWithOneConnection)
{
    const DownloadId id = *Add("http://x.test/a.dat").id;
    const TransferFailure inconsistent{FailureReason::InconsistentContent, FailureClass::NeedsUser, 0, "overlap"};
    Finish(id, 1, TransferOutcome::Failed, {{0, 100, 50}}, inconsistent, 100);
    ASSERT_EQ(engine_.starts.size(), 2u);
    EXPECT_EQ(engine_.starts.back().maxConnections, 1);
    EXPECT_TRUE(engine_.starts.back().segments.empty()) << "bytes from two answers must never be mixed";

    Finish(id, 2, TransferOutcome::Failed, {}, inconsistent, 100);
    EXPECT_EQ(engine_.starts.size(), 2u);
    EXPECT_EQ(View(id).download.status, DownloadStatus::Failed);
    EXPECT_EQ(observer_.All<ldm::core::DownloadFailed>().size(), 1u);
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
    ASSERT_EQ(engine_.finalizes.size(), 1u);
    EXPECT_EQ(engine_.finalizes.back().finalPath, kRoot / L"Elsewhere" / L"renamed.mkv");
    EXPECT_FALSE(View(id).awaitingConfirmation);
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
