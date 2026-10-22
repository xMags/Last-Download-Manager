// The full core stack against the HTTP fixture: core thread, coordinator, real engine, SQLite on
// disk and the Windows file system. Covers design section 9 items 2 and 3: correct files after
// pause, resume and restart, and seeded random command races.
#include "FixtureServer.h"

#include "base/Utf.h"
#include "core/CoreThread.h"
#include "engine/CurlEngine.h"
#include "platform/win/SystemPorts.h"
#include "storage/DpapiSecretProtector.h"
#include "storage/SqliteStore.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>

namespace {

using std::chrono_literals::operator""ms;
using std::chrono_literals::operator""s;
using ldm::model::DownloadId;
using ldm::model::DownloadStatus;
namespace fs = std::filesystem;

class NoProxy final : public ldm::core::ProxyResolver {
public:
    void Configure(const ldm::model::Settings&) override {}
    std::optional<std::string> ProxyFor(const std::string&) override { return std::nullopt; }
};

class WaitingObserver final : public ldm::core::CoreObserver {
public:
    void OnSnapshot(std::shared_ptr<const ldm::core::Snapshot> snapshot) override
    {
        {
            std::lock_guard lock(mutex_);
            snapshot_ = std::move(snapshot);
        }
        changed_.notify_all();
    }
    void OnNotification(ldm::core::Notification notification) override
    {
        {
            std::lock_guard lock(mutex_);
            notifications_.push_back(std::move(notification));
        }
        changed_.notify_all();
    }

    // Waits until `predicate` holds for the newest snapshot.
    bool WaitSnapshot(const std::function<bool(const ldm::core::Snapshot&)>& predicate, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return snapshot_ && predicate(*snapshot_); });
    }

    std::optional<ldm::core::AddResult> WaitAdd(std::uint64_t token, std::chrono::milliseconds timeout = 10s)
    {
        std::unique_lock lock(mutex_);
        std::optional<ldm::core::AddResult> found;
        changed_.wait_for(lock, timeout, [&] {
            for (const auto& notification : notifications_) {
                if (const auto* result = std::get_if<ldm::core::AddResult>(&notification); result && result->token == token) {
                    found = *result;
                    return true;
                }
            }
            return false;
        });
        return found;
    }

    std::shared_ptr<const ldm::core::Snapshot> Latest()
    {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }

    std::vector<std::string> Errors()
    {
        std::lock_guard lock(mutex_);
        std::vector<std::string> errors;
        for (const auto& notification : notifications_) {
            if (const auto* error = std::get_if<ldm::core::CoreError>(&notification)) {
                errors.push_back(error->message);
            }
        }
        return errors;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::shared_ptr<const ldm::core::Snapshot> snapshot_;
    std::vector<ldm::core::Notification> notifications_;
};

// One running instance of the core: what ldm.exe builds in its composition root, minus the UI.
class CoreStack {
public:
    CoreStack(const fs::path& database, const fs::path& downloads)
    {
        auto store = ldm::storage::SqliteStore::Open(ldm::base::ToUtf8(database.wstring()));
        if (!store) {
            throw std::runtime_error(store.GetError().message);
        }
        store_ = std::make_unique<ldm::storage::SqliteStore>(std::move(store).Value());
        ldm::engine::EngineOptions options;
        options.flushInterval = 200ms;
        engine_ = std::make_unique<ldm::engine::CurlEngine>(core_, options);
        ldm::core::CoordinatorPorts ports;
        ports.store = store_.get();
        ports.engine = engine_.get();
        ports.observer = &observer;
        ports.clock = &clock_;
        ports.fileSystem = &files_;
        ports.secrets = &secrets_;
        ports.ids = &ids_;
        ports.power = &power_;
        ports.proxy = &proxy_;
        ldm::core::CoordinatorConfig config;
        config.downloadsFolder = downloads;
        config.checkpointInterval = 300ms;
        coordinator_ = std::make_unique<ldm::core::Coordinator>(ports, config);
        const auto started = core_.Start(*coordinator_);
        if (!started) {
            throw std::runtime_error(started.GetError().message);
        }
    }

    ~CoreStack() { Stop(10s); }

    void Stop(std::chrono::milliseconds timeout)
    {
        if (stopped_) {
            return;
        }
        stopped_ = true;
        core_.Shutdown(timeout);  // the order of design section 3: core first, then the engine
        engine_.reset();
        coordinator_.reset();
        store_.reset();
    }

    void Post(ldm::core::Command command) { core_.Post(std::move(command)); }

    std::uint64_t Add(const std::string& url, ldm::core::AddMode mode = ldm::core::AddMode::StartNow)
    {
        ldm::core::AddDownloadCommand command;
        command.token = ++token_;
        command.url = url;
        command.mode = mode;
        command.duplicatePolicy = ldm::model::DuplicatePolicy::AddNumbered;
        Post(command);
        return command.token;
    }

    WaitingObserver observer;

private:
    ldm::core::CoreThread core_;  // first: the engine reports into it until it is destroyed
    ldm::platform::SystemClock clock_;
    ldm::platform::WinFileSystem files_;
    ldm::storage::DpapiSecretProtector secrets_;
    ldm::platform::RandomIdGenerator ids_;
    ldm::platform::PowerRequest power_;
    NoProxy proxy_;
    std::unique_ptr<ldm::storage::SqliteStore> store_;
    std::unique_ptr<ldm::engine::CurlEngine> engine_;
    std::unique_ptr<ldm::core::Coordinator> coordinator_;
    std::uint64_t token_ = 0;
    bool stopped_ = false;
};

const ldm::core::DownloadView* FindView(const ldm::core::Snapshot& snapshot, DownloadId id)
{
    for (const auto& view : snapshot.downloads) {
        if (view->download.id == id) {
            return view.get();
        }
    }
    return nullptr;
}

class CoreTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        ldm::test::FixtureOptions options;
        options.size = 3 * 1024 * 1024;
        options.delayMs = 10;
        server_ = ldm::test::FixtureServer::Launch(options).release();
    }
    static void TearDownTestSuite()
    {
        delete server_;
        server_ = nullptr;
    }

    void SetUp() override
    {
        if (server_ == nullptr) {
            GTEST_SKIP() << "Node.js is not installed; fixture tests skipped";
        }
        std::random_device random;
        root_ = fs::temp_directory_path() / ("ldm-core-test-" + std::to_string(random()));
        fs::create_directories(root_ / "downloads");
    }

    void TearDown() override
    {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    std::unique_ptr<CoreStack> Launch() { return std::make_unique<CoreStack>(root_ / "ldm.sqlite3", root_ / "downloads"); }

    ::testing::AssertionResult HoldsPayload(const fs::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            return ::testing::AssertionFailure() << "missing " << path.string();
        }
        const std::vector<char> bytes((std::istreambuf_iterator<char>(stream)), {});
        if (static_cast<std::int64_t>(bytes.size()) != server_->Size()) {
            return ::testing::AssertionFailure() << path.string() << " has " << bytes.size() << " bytes";
        }
        for (size_t i = 0; i < bytes.size(); ++i) {
            if (static_cast<std::uint8_t>(bytes[i]) != ldm::test::FixtureByte(static_cast<std::int64_t>(i))) {
                return ::testing::AssertionFailure() << path.string() << " differs at byte " << i;
            }
        }
        return ::testing::AssertionSuccess();
    }

    std::vector<fs::path> PartialFiles()
    {
        std::vector<fs::path> partials;
        for (const auto& entry : fs::recursive_directory_iterator(root_ / "downloads")) {
            if (entry.path().extension() == ".ldmpart") {
                partials.push_back(entry.path());
            }
        }
        return partials;
    }

    static ldm::test::FixtureServer* server_;
    fs::path root_;
};

ldm::test::FixtureServer* CoreTest::server_ = nullptr;

TEST_F(CoreTest, AddDownloadsToTheCategoryFolder)
{
    auto stack = Launch();
    const auto added = stack->observer.WaitAdd(stack->Add(server_->Url("/range.bin")));
    ASSERT_TRUE(added && added->id);
    const DownloadId id = *added->id;
    ASSERT_TRUE(stack->observer.WaitSnapshot(
        [&](const auto& snapshot) {
            const auto* view = FindView(snapshot, id);
            return view && view->download.status == DownloadStatus::Completed;
        },
        60s));
    // ".bin" belongs to the Programs category.
    EXPECT_TRUE(HoldsPayload(root_ / "downloads" / "Programs" / "range.bin"));
    EXPECT_TRUE(PartialFiles().empty());
    EXPECT_TRUE(stack->observer.Errors().empty());
}

TEST_F(CoreTest, StopRestartAndResumeAcrossProcessLifetimes)
{
    DownloadId id;
    {
        auto stack = Launch();
        stack->Post(ldm::core::SetSpeedLimiterCommand{true, 512});  // stop lands mid-transfer
        const auto added = stack->observer.WaitAdd(stack->Add(server_->Url("/range.bin")));
        ASSERT_TRUE(added && added->id);
        id = *added->id;
        ASSERT_TRUE(stack->observer.WaitSnapshot(
            [&](const auto& snapshot) {
                const auto* view = FindView(snapshot, id);
                return view && view->receivedBytes > 1024 * 1024;
            },
            30s));
        stack->Stop(10s);  // exit with the download running (checklist V-04)
    }
    {
        auto stack = Launch();
        const auto snapshot = stack->observer.Latest();
        ASSERT_TRUE(snapshot);
        const auto* view = FindView(*snapshot, id);
        ASSERT_NE(view, nullptr);
        ASSERT_EQ(view->download.status, DownloadStatus::Paused);
        EXPECT_GT(view->download.DownloadedBytes(), 0);
        EXPECT_LT(view->download.DownloadedBytes(), server_->Size());

        stack->Post(ldm::core::ResumeCommand{{id}});
        ASSERT_TRUE(stack->observer.WaitSnapshot(
            [&](const auto& s) {
                const auto* v = FindView(s, id);
                return v && v->download.status == DownloadStatus::Completed;
            },
            60s));
    }
    EXPECT_TRUE(HoldsPayload(root_ / "downloads" / "Programs" / "range.bin"));
}

TEST_F(CoreTest, AbruptExitKeepsOnlyDurableProgress)
{
    DownloadId id;
    {
        auto stack = Launch();
        stack->Post(ldm::core::SetSpeedLimiterCommand{true, 512});  // stop lands mid-transfer
        const auto added = stack->observer.WaitAdd(stack->Add(server_->Url("/range.bin")));
        ASSERT_TRUE(added && added->id);
        id = *added->id;
        ASSERT_TRUE(stack->observer.WaitSnapshot(
            [&](const auto& snapshot) {
                const auto* view = FindView(snapshot, id);
                return view && view->download.DownloadedBytes() > 512 * 1024;
            },
            30s));
        // No grace period: the core writes its last checkpoint while the engine is still
        // writing, which is what a forced exit leaves behind.
        stack->Stop(0ms);
    }
    {
        auto stack = Launch();
        const auto restored = stack->observer.Latest();
        ASSERT_TRUE(restored);
        const auto* view = FindView(*restored, id);
        ASSERT_NE(view, nullptr);
        ASSERT_EQ(view->download.status, DownloadStatus::Paused);
        EXPECT_LT(view->download.DownloadedBytes(), server_->Size());
        stack->Post(ldm::core::ResumeCommand{{id}});
        ASSERT_TRUE(stack->observer.WaitSnapshot(
            [&](const auto& s) {
                const auto* v = FindView(s, id);
                return v && v->download.status == DownloadStatus::Completed;
            },
            60s));
    }
    EXPECT_TRUE(HoldsPayload(root_ / "downloads" / "Programs" / "range.bin"));
}

// Seeded random stop, resume, delete and redownload commands during transfers. Whatever the
// interleaving, every surviving download must finish with the exact payload and every deleted one
// must leave no file behind.
TEST_F(CoreTest, RandomCommandRacesAreSafe)
{
    for (const unsigned seed : {1u, 2u, 3u}) {
        SCOPED_TRACE("seed " + std::to_string(seed));
        std::mt19937 random(seed);
        auto stack = Launch();
        stack->Post(ldm::core::SetSpeedLimiterCommand{true, 4096});  // keep commands landing mid-transfer
        std::vector<DownloadId> ids;
        for (int i = 0; i < 4; ++i) {
            const auto added = stack->observer.WaitAdd(stack->Add(server_->Url("/range.bin")));
            ASSERT_TRUE(added && added->id);
            ids.push_back(*added->id);
        }
        std::set<DownloadId> deleted;
        for (int step = 0; step < 25; ++step) {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::uniform_int_distribution<int>(0, 120)(random)));
            const DownloadId target = ids[std::uniform_int_distribution<size_t>(0, ids.size() - 1)(random)];
            if (deleted.contains(target)) {
                continue;
            }
            switch (std::uniform_int_distribution<int>(0, 9)(random)) {
            case 0:
                stack->Post(ldm::core::DeleteCommand{{target}, true});
                deleted.insert(target);
                break;
            case 1:
                stack->Post(ldm::core::RedownloadCommand{target});
                break;
            case 2:
            case 3:
            case 4:
            case 5:
                stack->Post(ldm::core::StopCommand{{target}});
                break;
            default:
                stack->Post(ldm::core::ResumeCommand{{target}});
                break;
            }
        }
        std::vector<DownloadId> survivors;
        for (const DownloadId id : ids) {
            if (!deleted.contains(id)) {
                survivors.push_back(id);
            }
        }
        stack->Post(ldm::core::SetSpeedLimiterCommand{false, std::nullopt});
        stack->Post(ldm::core::ResumeCommand{survivors});
        const bool settled = stack->observer.WaitSnapshot(
            [&](const auto& snapshot) {
                for (const DownloadId id : deleted) {
                    if (FindView(snapshot, id) != nullptr) {
                        return false;
                    }
                }
                for (const DownloadId id : survivors) {
                    const auto* view = FindView(snapshot, id);
                    if (view == nullptr || view->download.status != DownloadStatus::Completed) {
                        return false;
                    }
                }
                return true;
            },
            120s);
        ASSERT_TRUE(settled);
        const auto snapshot = stack->observer.Latest();
        for (const DownloadId id : survivors) {
            const auto* view = FindView(*snapshot, id);
            EXPECT_TRUE(HoldsPayload(fs::path(ldm::base::ToUtf16(view->download.saveDir)) /
                                     ldm::base::ToUtf16(view->download.fileName)));
        }
        EXPECT_TRUE(PartialFiles().empty());
        EXPECT_TRUE(stack->observer.Errors().empty());
        stack.reset();
        std::error_code ignored;
        fs::remove_all(root_ / "downloads", ignored);
        fs::remove(root_ / "ldm.sqlite3", ignored);
        fs::remove(root_ / "ldm.sqlite3-wal", ignored);
        fs::remove(root_ / "ldm.sqlite3-shm", ignored);
        fs::create_directories(root_ / "downloads");
    }
}

}  // namespace
