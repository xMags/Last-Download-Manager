// The full core stack against the HTTP fixture: core thread, coordinator, real engine, SQLite on
// disk and the Windows file system. Covers design section 9 items 2 and 3: correct files after
// pause, resume, restart and a forced process kill, and seeded random command races.
#include "FixtureServer.h"

#include "base/Utf.h"
#include "core/CoreThread.h"
#include "engine/CurlEngine.h"
#include "platform/win/SystemPorts.h"
#include "storage/DpapiSecretProtector.h"
#include "storage/SqliteStore.h"

#include <gtest/gtest.h>
#include <windows.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string_view>
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
    ldm::core::ProxyLookup Resolve(ldm::model::DownloadId, std::int64_t, const std::string&) override { return {}; }
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

// Production keeps ldm.exe's flush and checkpoint intervals (2 seconds each); Fast shortens them so
// tests reach durable progress sooner.
enum class Timing { Fast, Production };

// One running instance of the core: what ldm.exe builds in its composition root, minus the UI.
class CoreStack {
public:
    CoreStack(const fs::path& database, const fs::path& downloads, Timing timing = Timing::Fast)
    {
        auto store = ldm::storage::SqliteStore::Open(ldm::base::ToUtf8(database.wstring()));
        if (!store) {
            throw std::runtime_error(store.GetError().message);
        }
        store_ = std::make_unique<ldm::storage::SqliteStore>(std::move(store).Value());
        ldm::engine::EngineOptions options;
        if (timing == Timing::Fast) {
            options.flushInterval = 200ms;
        }
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
        if (timing == Timing::Fast) {
            config.checkpointInterval = 300ms;
        }
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

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~UniqueHandle() { Reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    void Reset() noexcept
    {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
        handle_ = nullptr;
    }
    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
    [[nodiscard]] HANDLE* Out() noexcept { return &handle_; }

private:
    HANDLE handle_;
};

// The forced-kill test starts a second copy of this executable that runs only the worker test
// below, then kills it. The worker learns its folder and address from these variables and reports
// its durable progress on stdout in lines starting with kWorkerPrefix.
constexpr wchar_t kWorkerRootVariable[] = L"LDM_KILL_WORKER_ROOT";
constexpr wchar_t kWorkerUrlVariable[] = L"LDM_KILL_WORKER_URL";
constexpr std::string_view kWorkerPrefix = "ldm-kill-worker durable=";
constexpr UINT kKilledExitCode = 0xDEAD;

std::optional<std::wstring> EnvironmentValue(const wchar_t* name)
{
    const DWORD length = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (length == 0) {
        return std::nullopt;
    }
    std::wstring value(length, L'\0');
    value.resize(::GetEnvironmentVariableW(name, value.data(), length));
    return value;
}

// Disabled, so it runs only when CoreTest.ForcedKillAndRestartResumes starts it in a child process.
TEST(CoreKillWorker, DISABLED_Run)
{
    const auto root = EnvironmentValue(kWorkerRootVariable);
    const auto url = EnvironmentValue(kWorkerUrlVariable);
    if (!root || !url) {
        GTEST_SKIP() << "runs only as the child process of CoreTest.ForcedKillAndRestartResumes";
    }
    // ldm.exe's own intervals, so the kill leaves what a real crash would: a checkpoint up to two
    // seconds old and written but unflushed data in the partial file.
    CoreStack stack(fs::path(*root) / "ldm.sqlite3", fs::path(*root) / "downloads", Timing::Production);
    stack.Post(ldm::core::SetSpeedLimiterCommand{true, 256});  // the kill lands mid-transfer
    const auto added = stack.observer.WaitAdd(stack.Add(ldm::base::ToUtf8(*url)));
    ASSERT_TRUE(added && added->id);
    const DownloadId id = *added->id;
    std::int64_t reported = -1;
    const auto deadline = std::chrono::steady_clock::now() + 120s;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)stack.observer.WaitSnapshot(
            [id, reported](const auto& snapshot) {
                const auto* view = FindView(snapshot, id);
                return view && view->download.DownloadedBytes() != reported;
            },
            1s);
        const auto snapshot = stack.observer.Latest();
        const auto* view = snapshot ? FindView(*snapshot, id) : nullptr;
        if (view == nullptr || view->download.DownloadedBytes() == reported) {
            continue;
        }
        ASSERT_NE(view->download.status, DownloadStatus::Completed) << "finished before the kill";
        reported = view->download.DownloadedBytes();
        std::printf("%.*s%lld\n", static_cast<int>(kWorkerPrefix.size()), kWorkerPrefix.data(),
                    static_cast<long long>(reported));
        std::fflush(stdout);
    }
    FAIL() << "the worker was not killed";
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

// Design section 9 item 2 and checklist ENG-09: the process doing the work is killed with
// TerminateProcess, so none of its shutdown code runs, then the core restarts on the same database
// and folder, resumes, and must produce the exact payload.
TEST_F(CoreTest, ForcedKillAndRestartResumes)
{
    std::wstring self(MAX_PATH, L'\0');
    self.resize(::GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size())));
    ASSERT_FALSE(self.empty());

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    UniqueHandle stdoutRead;
    UniqueHandle stdoutWrite;
    ASSERT_TRUE(::CreatePipe(stdoutRead.Out(), stdoutWrite.Out(), &inherit, 0));
    ::SetHandleInformation(stdoutRead.Get(), HANDLE_FLAG_INHERIT, 0);

    // The job kills the worker if this test fails before it does.
    UniqueHandle job(::CreateJobObjectW(nullptr, nullptr));
    ASSERT_NE(job.Get(), nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ASSERT_TRUE(::SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)));

    std::wstring commandLine =
        L"\"" + self + L"\" --gtest_filter=CoreKillWorker.DISABLED_Run --gtest_also_run_disabled_tests";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = stdoutWrite.Get();
    startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION started{};
    ::SetEnvironmentVariableW(kWorkerRootVariable, root_.c_str());
    ::SetEnvironmentVariableW(kWorkerUrlVariable, ldm::base::ToUtf16(server_->Url("/range.bin")).c_str());
    const BOOL created = ::CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                                          CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &started);
    const DWORD createError = ::GetLastError();
    ::SetEnvironmentVariableW(kWorkerRootVariable, nullptr);
    ::SetEnvironmentVariableW(kWorkerUrlVariable, nullptr);
    ASSERT_TRUE(created) << "could not start the worker, error " << createError;
    UniqueHandle worker(started.hProcess);
    UniqueHandle workerThread(started.hThread);
    ASSERT_TRUE(::AssignProcessToJobObject(job.Get(), worker.Get()));
    ::ResumeThread(workerThread.Get());
    stdoutWrite.Reset();  // only the worker holds the write end now, so its exit ends the pipe

    // Kill once at least 1 MiB is durable, so the restart has real progress to resume from.
    std::string output;
    std::int64_t durable = 0;
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (durable < 1024 * 1024 && std::chrono::steady_clock::now() < deadline) {
        DWORD available = 0;
        if (!::PeekNamedPipe(stdoutRead.Get(), nullptr, 0, nullptr, &available, nullptr)) {
            break;  // the worker exited on its own
        }
        if (available == 0) {
            std::this_thread::sleep_for(20ms);
            continue;
        }
        char chunk[4096];
        DWORD read = 0;
        if (!::ReadFile(stdoutRead.Get(), chunk, std::min<DWORD>(available, sizeof(chunk)), &read, nullptr)) {
            break;
        }
        output.append(chunk, read);
        for (size_t at = output.find(kWorkerPrefix); at != std::string::npos; at = output.find(kWorkerPrefix, at + 1)) {
            const char* begin = output.data() + at + kWorkerPrefix.size();
            std::int64_t value = 0;
            if (std::from_chars(begin, output.data() + output.size(), value).ec == std::errc()) {
                durable = std::max(durable, value);
            }
        }
    }
    ASSERT_GE(durable, 1024 * 1024) << "worker output:\n" << output;
    ASSERT_TRUE(::TerminateProcess(worker.Get(), kKilledExitCode));
    ASSERT_EQ(::WaitForSingleObject(worker.Get(), 10000), WAIT_OBJECT_0);
    DWORD exitCode = 0;
    ASSERT_TRUE(::GetExitCodeProcess(worker.Get(), &exitCode));
    ASSERT_EQ(exitCode, kKilledExitCode) << "the worker ended on its own:\n" << output;
    EXPECT_FALSE(PartialFiles().empty());

    auto stack = Launch();
    const auto restored = stack->observer.Latest();
    ASSERT_TRUE(restored);
    ASSERT_EQ(restored->downloads.size(), 1u);
    const auto& download = restored->downloads.front()->download;
    const DownloadId id = download.id;
    EXPECT_EQ(download.status, DownloadStatus::Paused);
    EXPECT_GT(download.DownloadedBytes(), 0);
    EXPECT_LT(download.DownloadedBytes(), server_->Size());
    stack->Post(ldm::core::SetSpeedLimiterCommand{false, std::nullopt});  // the worker turned it on
    stack->Post(ldm::core::ResumeCommand{{id}});
    ASSERT_TRUE(stack->observer.WaitSnapshot(
        [id](const auto& s) {
            const auto* v = FindView(s, id);
            return v && v->download.status == DownloadStatus::Completed;
        },
        60s));
    const auto finished = stack->observer.Latest();
    const auto* view = FindView(*finished, id);
    ASSERT_NE(view, nullptr);
    EXPECT_TRUE(HoldsPayload(fs::path(ldm::base::ToUtf16(view->download.saveDir)) /
                             ldm::base::ToUtf16(view->download.fileName)));
    EXPECT_TRUE(PartialFiles().empty());
    EXPECT_TRUE(stack->observer.Errors().empty());
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

const ldm::core::QueueView* FindQueue(const ldm::core::Snapshot& snapshot, ldm::model::QueueKind kind)
{
    for (const auto& view : snapshot.queues) {
        if (view.queue.kind == kind) {
            return &view;
        }
    }
    return nullptr;
}

// M4 design section 8: a queue never runs more members at once than it allows.
TEST_F(CoreTest, AQueueRunsAtMostItsConcurrencyAtOnce)
{
    auto stack = Launch();
    ASSERT_TRUE(stack->observer.WaitSnapshot([](const auto& s) { return !s.queues.empty(); }, 10s));
    ldm::model::Queue main = FindQueue(*stack->observer.Latest(), ldm::model::QueueKind::Main)->queue;
    main.concurrent = 2;
    stack->Post(ldm::core::SaveQueueCommand{main});
    std::vector<DownloadId> ids;
    for (int i = 0; i < 5; ++i) {
        const auto added = stack->observer.WaitAdd(stack->Add(server_->Url("/range.bin"), ldm::core::AddMode::Later));
        ASSERT_TRUE(added && added->id);
        ids.push_back(*added->id);
    }
    stack->Post(ldm::core::StartQueueCommand{main.id});

    int mostAtOnce = 0;
    const bool done = stack->observer.WaitSnapshot(
        [&](const ldm::core::Snapshot& snapshot) {
            const auto* queue = FindQueue(snapshot, ldm::model::QueueKind::Main);
            int downloading = 0;
            int completed = 0;
            for (const DownloadId id : ids) {
                const auto* view = FindView(snapshot, id);
                downloading += view && view->download.status == DownloadStatus::Downloading ? 1 : 0;
                completed += view && view->download.status == DownloadStatus::Completed ? 1 : 0;
            }
            mostAtOnce = std::max({mostAtOnce, downloading, queue ? queue->runningMembers : 0});
            return completed == 5 && queue && !queue->running;
        },
        120s);
    ASSERT_TRUE(done) << "the queue did not finish";
    EXPECT_EQ(mostAtOnce, 2);
    EXPECT_EQ(FindQueue(*stack->observer.Latest(), ldm::model::QueueKind::Main)->members, 0)
        << "completed members leave the queue";
    EXPECT_TRUE(PartialFiles().empty());
    EXPECT_TRUE(stack->observer.Errors().empty());
}

// M4 design section 5: the synchronization queue replaces a file that changed on the server and
// leaves an unchanged one alone.
TEST_F(CoreTest, TheSynchronizationQueueReplacesOnlyAChangedFile)
{
    auto stack = Launch();
    const auto added = stack->observer.WaitAdd(stack->Add(server_->Url("/versioned.bin")));
    ASSERT_TRUE(added && added->id);
    const DownloadId id = *added->id;
    const auto completed = [id](const ldm::core::Snapshot& snapshot) {
        const auto* view = FindView(snapshot, id);
        return view && view->download.status == DownloadStatus::Completed;
    };
    ASSERT_TRUE(stack->observer.WaitSnapshot(completed, 60s));
    const fs::path file = root_ / "downloads" / "Programs" / "versioned.bin";
    const auto read = [&file] {
        std::ifstream stream(file, std::ios::binary);
        return std::vector<char>((std::istreambuf_iterator<char>(stream)), {});
    };
    const auto original = read();
    ASSERT_EQ(static_cast<std::int64_t>(original.size()), server_->Size());
    const auto sync = FindQueue(*stack->observer.Latest(), ldm::model::QueueKind::Sync)->queue.id;
    stack->Post(ldm::core::SetQueueCommand{{id}, sync});

    stack->Post(ldm::core::StartQueueCommand{sync});
    ASSERT_TRUE(stack->observer.WaitSnapshot(
        [id](const ldm::core::Snapshot& snapshot) {
            const auto* view = FindView(snapshot, id);
            const auto* queue = FindQueue(snapshot, ldm::model::QueueKind::Sync);
            return view && view->download.lastResult == "Unchanged on the server" && queue && !queue->running;
        },
        30s));
    EXPECT_EQ(read(), original);

    server_->Bump();
    const auto firstCompletion = FindView(*stack->observer.Latest(), id)->download.completedAtMs;
    stack->Post(ldm::core::StartQueueCommand{sync});
    ASSERT_TRUE(stack->observer.WaitSnapshot(
        [id, firstCompletion](const ldm::core::Snapshot& snapshot) {
            const auto* view = FindView(snapshot, id);
            const auto* queue = FindQueue(snapshot, ldm::model::QueueKind::Sync);
            return view && view->download.status == DownloadStatus::Completed &&
                   view->download.completedAtMs != firstCompletion && queue && !queue->running;
        },
        60s));
    const auto replaced = read();
    EXPECT_EQ(static_cast<std::int64_t>(replaced.size()), server_->Size());
    EXPECT_NE(replaced, original) << "the changed file replaced the old one";
    EXPECT_EQ(FindView(*stack->observer.Latest(), id)->download.queueId, sync);
    EXPECT_TRUE(PartialFiles().empty());
    EXPECT_TRUE(stack->observer.Errors().empty());
}

}  // namespace
