// Engine against the HTTP fixture (design section 9). Each test drives the real CurlEngine,
// network and disk threads included, and checks the file byte by byte.
#include "FixtureServer.h"
#include "RecordingListener.h"

#include "engine/CurlEngine.h"
#include "model/Cookie.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <vector>

namespace {

using std::chrono_literals::operator""ms;
using std::chrono_literals::operator""s;
using ldm::core::EngineEvent;
using ldm::core::FailureReason;
using ldm::core::ProgressInfo;
using ldm::core::ResponseInfo;
using ldm::core::TransferFinished;
using ldm::core::TransferOutcome;
using ldm::core::TransferRequest;
using ldm::model::DownloadId;
using ldm::test::FixtureServer;
using ldm::test::RecordingListener;

ldm::engine::EngineOptions TestOptions()
{
    ldm::engine::EngineOptions options;
    options.flushInterval = 100ms;
    options.tuning.progressInterval = 50ms;
    options.shutdownTimeout = 5s;
    return options;
}

class EngineTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        ldm::test::FixtureOptions options;
        options.size = 4 * 1024 * 1024;
        options.delayMs = 10;  // about 3 MB/s per connection, so segmentation is observable
        server_ = FixtureServer::Launch(options).release();
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
        server_->Reset();
        std::random_device random;
        directory_ = std::filesystem::temp_directory_path() / ("ldm-engine-test-" + std::to_string(random()));
        std::filesystem::create_directories(directory_);
        engine_ = std::make_unique<ldm::engine::CurlEngine>(listener_, TestOptions());
    }

    void TearDown() override
    {
        engine_.reset();
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    // Each download id gets its own partial file, so several runs can share one test.
    TransferRequest Request(const std::string& path, std::int64_t epoch = 1, std::int64_t id = 1)
    {
        TransferRequest request;
        request.id = DownloadId{id};
        request.epoch = epoch;
        request.url = server_->Url(path);
        request.userAgent = "LDM-Test/1.0";
        request.partialPath = PartialPath(id);
        return request;
    }

    std::filesystem::path PartialPath(std::int64_t id = 1) const
    {
        return directory_ / ("file" + std::to_string(id) + ".ldmpart");
    }

    // Starts `request` and waits for its TransferFinished.
    TransferFinished Run(TransferRequest request, std::chrono::milliseconds timeout = 60s)
    {
        const DownloadId id = request.id;
        const std::int64_t epoch = request.epoch;
        engine_->Start(std::move(request));
        auto finished = listener_.Finished(id, epoch, timeout);
        EXPECT_TRUE(finished) << "no TransferFinished within the timeout";
        return finished.value_or(TransferFinished{});
    }

    std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream), {});
    }

    // True when the partial file holds exactly the fixture payload.
    ::testing::AssertionResult HoldsPayload(const std::filesystem::path& path, std::int64_t size)
    {
        const auto bytes = ReadFile(path);
        if (static_cast<std::int64_t>(bytes.size()) != size) {
            return ::testing::AssertionFailure() << "size " << bytes.size() << ", expected " << size;
        }
        for (std::int64_t i = 0; i < size; ++i) {
            if (bytes[static_cast<size_t>(i)] != ldm::test::FixtureByte(i)) {
                return ::testing::AssertionFailure() << "byte " << i << " differs";
            }
        }
        return ::testing::AssertionSuccess();
    }

    std::vector<nlohmann::json> Requests(const std::string& path)
    {
        std::vector<nlohmann::json> result;
        const nlohmann::json log = server_->Log();  // a temporary in the range-for would dangle
        for (const auto& entry : log.at("entries")) {
            if (entry.value("event", "") == "request" && entry.value("path", "") == path) {
                result.push_back(entry);
            }
        }
        return result;
    }

    // FTP transfers the fixture logged (ENG-13): one entry per RETR, with its REST offset.
    std::vector<nlohmann::json> FtpRetrievals()
    {
        std::vector<nlohmann::json> result;
        const nlohmann::json log = server_->Log();
        for (const auto& entry : log.at("entries")) {
            if (entry.value("event", "") == "ftp-retr") {
                result.push_back(entry);
            }
        }
        return result;
    }

    TransferRequest FtpRequest(const std::string& user = {}, std::int64_t epoch = 1)
    {
        TransferRequest request = Request("/range.bin", epoch);
        request.url = server_->FtpUrl("/range.bin", user);
        return request;
    }

    static FixtureServer* server_;
    std::filesystem::path directory_;
    RecordingListener listener_;
    std::unique_ptr<ldm::engine::CurlEngine> engine_;
};

FixtureServer* EngineTest::server_ = nullptr;

void ExpectCovers(const std::vector<ldm::model::Segment>& segments, std::int64_t size)
{
    std::int64_t expectedStart = 0;
    for (const auto& segment : segments) {
        EXPECT_EQ(segment.start, expectedStart);
        ASSERT_TRUE(segment.end.has_value());
        EXPECT_TRUE(segment.IsComplete());
        expectedStart = *segment.end;
    }
    EXPECT_EQ(expectedStart, size);
}

TEST_F(EngineTest, SegmentedDownloadMatchesPayloadWithPlainGetThenRanges)
{
    const auto finished = Run(Request("/range.bin"));
    ASSERT_EQ(finished.outcome, TransferOutcome::Completed) << (finished.failure ? finished.failure->message : "");
    EXPECT_EQ(finished.totalSize, server_->Size());
    ExpectCovers(finished.segments, server_->Size());
    EXPECT_GT(finished.segments.size(), 3u);
    EXPECT_TRUE(HoldsPayload(PartialPath(), server_->Size()));

    const auto requests = Requests("/range.bin");
    ASSERT_GE(requests.size(), 4u);
    // ENG-01: the first request is a plain GET; the following ones carry ranges and If-Range.
    EXPECT_TRUE(requests.front().at("range").is_null());
    bool bounded = false;
    for (size_t i = 1; i < requests.size(); ++i) {
        ASSERT_TRUE(requests[i].at("range").is_string());
        const std::string range = requests[i].at("range");
        bounded = bounded || range.back() != '-';
        EXPECT_TRUE(requests[i].at("ifRange").is_string());
    }
    EXPECT_TRUE(bounded) << "expected bounded follow-up ranges (ENG-02)";
    // ENG-12: headers of a manual download.
    EXPECT_EQ(requests.front().at("acceptEncoding"), "identity");
    EXPECT_EQ(requests.front().at("userAgent"), "LDM-Test/1.0");
    EXPECT_LE(server_->Log().at("maxActive").get<int>(), 8);

    const auto info = listener_.WaitFor<ResponseInfo>(DownloadId{1}, 1, 1s);
    ASSERT_TRUE(info);
    EXPECT_TRUE(info->resumable);
    EXPECT_EQ(info->fileName, "range.bin");
    EXPECT_TRUE(info->validators.etag.has_value());
}

TEST_F(EngineTest, ConnectionLimitIsHonored)
{
    auto request = Request("/range.bin");
    request.maxConnections = 2;
    const auto finished = Run(std::move(request));
    ASSERT_EQ(finished.outcome, TransferOutcome::Completed);
    EXPECT_LE(server_->Log().at("maxActive").get<int>(), 2);
    EXPECT_TRUE(HoldsPayload(PartialPath(), server_->Size()));
}

TEST_F(EngineTest, ServerWithoutRangesGetsExactlyOneConnection)
{
    const auto finished = Run(Request("/no-range.bin"));
    ASSERT_EQ(finished.outcome, TransferOutcome::Completed);
    EXPECT_EQ(Requests("/no-range.bin").size(), 1u);
    EXPECT_TRUE(HoldsPayload(PartialPath(), server_->Size()));
    const auto info = listener_.WaitFor<ResponseInfo>(DownloadId{1}, 1, 1s);
    ASSERT_TRUE(info);
    EXPECT_FALSE(info->resumable);
}

TEST_F(EngineTest, RedirectReportsFinalUrlAndServerName)
{
    const auto finished = Run(Request("/redirect"));
    ASSERT_EQ(finished.outcome, TransferOutcome::Completed);
    const auto info = listener_.WaitFor<ResponseInfo>(DownloadId{1}, 1, 1s);
    ASSERT_TRUE(info);
    EXPECT_TRUE(info->finalUrl.ends_with("/range.bin"));
    EXPECT_EQ(info->fileName, "range.bin");
    EXPECT_TRUE(HoldsPayload(PartialPath(), server_->Size()));
}

TEST_F(EngineTest, PauseAndResumeProducesIdenticalFile)
{
    engine_->Start(Request("/range.bin", 1));
    ASSERT_TRUE(listener_.WaitFor<ProgressInfo>(DownloadId{1}, 1, 30s, [](const ProgressInfo& progress) {
        return progress.receivedBytes > 1024 * 1024;
    }));
    engine_->Stop(DownloadId{1}, 1);
    const auto stopped = listener_.Finished(DownloadId{1}, 1);
    ASSERT_TRUE(stopped);
    ASSERT_EQ(stopped->outcome, TransferOutcome::Stopped);
    std::int64_t done = 0;
    for (const auto& segment : stopped->segments) {
        done += segment.done;
    }
    EXPECT_GT(done, 0);
    EXPECT_LT(done, server_->Size());

    const auto info = listener_.WaitFor<ResponseInfo>(DownloadId{1}, 1, 1s);
    ASSERT_TRUE(info);
    server_->Reset();
    auto resume = Request("/range.bin", 2);
    resume.segments = stopped->segments;
    resume.knownSize = stopped->totalSize;
    resume.resumable = info->resumable;
    resume.validators = info->validators;
    const auto finished = Run(std::move(resume));
    ASSERT_EQ(finished.outcome, TransferOutcome::Completed) << (finished.failure ? finished.failure->message : "");
    EXPECT_TRUE(HoldsPayload(PartialPath(), server_->Size()));
    for (const auto& request : Requests("/range.bin")) {
        EXPECT_TRUE(request.at("range").is_string()) << "a resumed run must never fetch from zero";
        EXPECT_TRUE(request.at("ifRange").is_string());
    }
}

TEST_F(EngineTest, FtpSegmentedDownloadMatchesPayload)
{
    const auto finished = Run(FtpRequest());
    ASSERT_EQ(finished.outcome, TransferOutcome::Completed) << (finished.failure ? finished.failure->message : "");
    EXPECT_EQ(finished.totalSize, server_->Size()) << "the size comes from SIZE";
    ExpectCovers(finished.segments, server_->Size());
    EXPECT_TRUE(HoldsPayload(PartialPath(), server_->Size()));
    const auto retrievals = FtpRetrievals();
    ASSERT_GT(retrievals.size(), 1u) << "extra connections use REST";
    EXPECT_EQ(retrievals.front().at("start"), 0);
    const auto info = listener_.WaitFor<ResponseInfo>(DownloadId{1}, 1, 1s);
    ASSERT_TRUE(info);
    EXPECT_TRUE(info->resumable);
}

TEST_F(EngineTest, FtpActiveModeDownloads)
{
    auto request = FtpRequest();
    request.ftpActiveMode = true;
    request.maxConnections = 2;
    const auto finished = Run(std::move(request));
    ASSERT_EQ(finished.outcome, TransferOutcome::Completed) << (finished.failure ? finished.failure->message : "");
    EXPECT_TRUE(HoldsPayload(PartialPath(), server_->Size()));
}

TEST_F(EngineTest, FtpPauseAndResume)
{
    engine_->Start(FtpRequest({}, 1));
    ASSERT_TRUE(listener_.WaitFor<ProgressInfo>(DownloadId{1}, 1, 30s, [](const ProgressInfo& progress) {
        return progress.receivedBytes > 1024 * 1024;
    }));
    engine_->Stop(DownloadId{1}, 1);
    const auto stopped = listener_.Finished(DownloadId{1}, 1);
    ASSERT_TRUE(stopped);
    ASSERT_EQ(stopped->outcome, TransferOutcome::Stopped);
    const auto info = listener_.WaitFor<ResponseInfo>(DownloadId{1}, 1, 1s);
    ASSERT_TRUE(info);

    server_->Reset();
    auto resume = FtpRequest({}, 2);
    resume.segments = stopped->segments;
    resume.knownSize = stopped->totalSize;
    resume.resumable = info->resumable;
    const auto finished = Run(std::move(resume));
    ASSERT_EQ(finished.outcome, TransferOutcome::Completed) << (finished.failure ? finished.failure->message : "");
    EXPECT_TRUE(HoldsPayload(PartialPath(), server_->Size()));
    for (const auto& retrieval : FtpRetrievals()) {
        EXPECT_GT(retrieval.at("start").get<std::int64_t>(), 0) << "a resumed run must never fetch from zero";
    }
}

TEST_F(EngineTest, FtpServerWithoutRestGetsOneConnection)
{
    const auto finished = Run(FtpRequest("norest"));
    ASSERT_EQ(finished.outcome, TransferOutcome::Completed) << (finished.failure ? finished.failure->message : "");
    EXPECT_TRUE(HoldsPayload(PartialPath(), server_->Size()));
    const auto info = listener_.WaitFor<ResponseInfo>(DownloadId{1}, 1, 1s, [](const ResponseInfo& response) {
        return !response.resumable;
    });
    EXPECT_TRUE(info) << "the download must be reported as not resumable";
}

TEST_F(EngineTest, ChangedResourceRefusesToResume)
{
    engine_->Start(Request("/versioned.bin", 1));
    ASSERT_TRUE(listener_.WaitFor<ProgressInfo>(DownloadId{1}, 1, 30s, [](const ProgressInfo& progress) {
        return progress.receivedBytes > 512 * 1024;
    }));
    engine_->Stop(DownloadId{1}, 1);
    const auto stopped = listener_.Finished(DownloadId{1}, 1);
    ASSERT_TRUE(stopped);
    const auto info = listener_.WaitFor<ResponseInfo>(DownloadId{1}, 1, 1s);
    ASSERT_TRUE(info);

    server_->Bump();
    auto resume = Request("/versioned.bin", 2);
    resume.segments = stopped->segments;
    resume.knownSize = stopped->totalSize;
    resume.resumable = info->resumable;
    resume.validators = info->validators;
    const auto finished = Run(std::move(resume));
    ASSERT_EQ(finished.outcome, TransferOutcome::Failed);
    ASSERT_TRUE(finished.failure);
    EXPECT_EQ(finished.failure->reason, FailureReason::ResourceChanged);
    // The stored progress comes back untouched, so the caller can decide to restart.
    EXPECT_EQ(finished.segments.size(), stopped->segments.size());
}

TEST_F(EngineTest, OverlapMismatchIsDetected)
{
    const auto finished = Run(Request("/changing.bin"));
    ASSERT_EQ(finished.outcome, TransferOutcome::Failed);
    ASSERT_TRUE(finished.failure);
    EXPECT_EQ(finished.failure->reason, FailureReason::InconsistentContent);
}

TEST_F(EngineTest, DroppedConnectionsAreRetried)
{
    const auto finished = Run(Request("/drop.bin?after=1048576"));
    ASSERT_EQ(finished.outcome, TransferOutcome::Completed) << (finished.failure ? finished.failure->message : "");
    EXPECT_TRUE(HoldsPayload(PartialPath(), server_->Size()));
    const auto progress = listener_.All<ProgressInfo>(DownloadId{1}, 1);
    const bool sawRetry = std::any_of(progress.begin(), progress.end(), [](const ProgressInfo& p) { return p.lastRetry.has_value(); });
    EXPECT_TRUE(sawRetry);
}

TEST_F(EngineTest, UnknownLengthStreamsToTheEnd)
{
    const auto finished = Run(Request("/unknown-length.bin"));
    ASSERT_EQ(finished.outcome, TransferOutcome::Completed);
    EXPECT_EQ(finished.totalSize, server_->Size());
    EXPECT_TRUE(HoldsPayload(PartialPath(), server_->Size()));
}

TEST_F(EngineTest, CompressedBodiesAreDecoded)
{
    std::int64_t id = 0;
    for (const std::string path : {"/gzip.bin", "/br.bin"}) {
        SCOPED_TRACE(path);
        ++id;
        const auto finished = Run(Request(path, 1, id));
        ASSERT_EQ(finished.outcome, TransferOutcome::Completed) << (finished.failure ? finished.failure->message : "");
        EXPECT_EQ(finished.totalSize, server_->Size());
        EXPECT_TRUE(HoldsPayload(PartialPath(id), server_->Size()));
    }
}

TEST_F(EngineTest, AuthenticationRequiredAndAccepted)
{
    const auto denied = Run(Request("/auth/basic.bin", 1));
    ASSERT_EQ(denied.outcome, TransferOutcome::Failed);
    EXPECT_EQ(denied.failure->reason, FailureReason::AuthRequired);

    std::int64_t id = 1;
    for (const std::string path : {"/auth/basic.bin", "/auth/digest.bin"}) {
        SCOPED_TRACE(path);
        ++id;
        auto request = Request(path, 1, id);
        request.credentials = ldm::core::TransferCredentials{"ldm", "secret"};
        const auto finished = Run(std::move(request));
        ASSERT_EQ(finished.outcome, TransferOutcome::Completed) << (finished.failure ? finished.failure->message : "");
        EXPECT_TRUE(HoldsPayload(PartialPath(id), server_->Size()));
    }
}

TEST_F(EngineTest, BrowserCookiesReachOnlyTheirOwnHostOnEveryConnection)
{
    const auto denied = Run(Request("/cookie.bin", 1));
    ASSERT_EQ(denied.outcome, TransferOutcome::Failed);

    auto request = Request("/cookie.bin", 1, 2);
    request.cookieLines = {
        ldm::model::ToNetscapeLine({"127.0.0.1", true, "/", false, 0, "sid", "abc"}),
        ldm::model::ToNetscapeLine({"other.test", false, "/", false, 0, "leak", "x"}),
    };
    const auto finished = Run(std::move(request));
    ASSERT_EQ(finished.outcome, TransferOutcome::Completed) << (finished.failure ? finished.failure->message : "");
    EXPECT_TRUE(HoldsPayload(PartialPath(2), server_->Size()));
}

// Manifest fetches (M3 design section 6): small GETs into memory beside the downloads.
ldm::core::FetchRequest FetchOf(const std::string& url, std::size_t maxBytes)
{
    ldm::core::FetchRequest request;
    request.fetchId = 7;
    request.url = url;
    request.userAgent = "LDM-Test";
    request.maxBytes = maxBytes;
    return request;
}

TEST_F(EngineTest, FetchReadsTheWholeBodyAfterRedirects)
{
    engine_->Fetch(FetchOf(server_->Url("/redirect"), 8 * 1024 * 1024));
    const auto fetched = listener_.WaitFor<ldm::core::FetchFinished>(DownloadId{}, 0, 60s);
    ASSERT_TRUE(fetched);
    EXPECT_EQ(fetched->fetchId, 7);
    ASSERT_FALSE(fetched->failure) << fetched->failure->message;
    EXPECT_TRUE(fetched->finalUrl.ends_with("/range.bin")) << fetched->finalUrl;
    EXPECT_EQ(fetched->body.size(), static_cast<size_t>(server_->Size()));
}

TEST_F(EngineTest, FetchDecodesCompressedBodies)
{
    engine_->Fetch(FetchOf(server_->Url("/gzip.bin"), 8 * 1024 * 1024));
    const auto fetched = listener_.WaitFor<ldm::core::FetchFinished>(DownloadId{}, 0, 60s);
    ASSERT_TRUE(fetched);
    ASSERT_FALSE(fetched->failure) << fetched->failure->message;
    EXPECT_EQ(fetched->body.size(), static_cast<size_t>(server_->Size()));
}

TEST_F(EngineTest, FetchRefusesBodiesOverItsLimit)
{
    engine_->Fetch(FetchOf(server_->Url("/unknown-length.bin"), 64 * 1024));  // no Content-Length to refuse early
    const auto fetched = listener_.WaitFor<ldm::core::FetchFinished>(DownloadId{}, 0, 60s);
    ASSERT_TRUE(fetched);
    ASSERT_TRUE(fetched->failure);
    EXPECT_EQ(fetched->failure->reason, FailureReason::Protocol);
    EXPECT_TRUE(fetched->body.empty());
}

TEST_F(EngineTest, FetchSendsCookiesAndReportsHttpErrors)
{
    engine_->Fetch(FetchOf(server_->Url("/cookie.bin"), 8 * 1024 * 1024));
    const auto denied = listener_.WaitFor<ldm::core::FetchFinished>(DownloadId{}, 0, 60s);
    ASSERT_TRUE(denied);
    ASSERT_TRUE(denied->failure);
    EXPECT_EQ(denied->failure->httpStatus, 403);

    auto request = FetchOf(server_->Url("/cookie.bin"), 8 * 1024 * 1024);
    request.fetchId = 8;
    request.cookieLines = {ldm::model::ToNetscapeLine({"127.0.0.1", true, "/", false, 0, "sid", "abc"})};
    engine_->Fetch(std::move(request));
    std::optional<ldm::core::FetchFinished> allowed;
    for (int i = 0; i < 600 && !allowed; ++i) {
        for (const auto& event : listener_.All<ldm::core::FetchFinished>(DownloadId{}, 0)) {
            if (event.fetchId == 8) {
                allowed = event;
            }
        }
        std::this_thread::sleep_for(100ms);
    }
    ASSERT_TRUE(allowed);
    EXPECT_FALSE(allowed->failure) << allowed->failure->message;
    EXPECT_EQ(allowed->body.size(), static_cast<size_t>(server_->Size()));
}

TEST_F(EngineTest, FormDownloadsSendTheFormAgain)
{
    const auto denied = Run(Request("/post.bin", 1));
    ASSERT_EQ(denied.outcome, TransferOutcome::Failed) << "a plain GET is refused";

    auto request = Request("/post.bin", 1, 2);
    request.post = ldm::core::PostData{"application/x-www-form-urlencoded", "token=abc&x=1"};
    request.maxConnections = 1;
    const auto finished = Run(std::move(request));
    ASSERT_EQ(finished.outcome, TransferOutcome::Completed) << (finished.failure ? finished.failure->message : "");
    EXPECT_TRUE(HoldsPayload(PartialPath(2), server_->Size()));
}

TEST_F(EngineTest, RetryAfterAndServiceUnavailableAreRetried)
{
    std::int64_t id = 0;
    for (const std::string path : {"/retry-after.bin", "/flaky.bin"}) {
        SCOPED_TRACE(path);
        ++id;
        const auto finished = Run(Request(path, 1, id));
        ASSERT_EQ(finished.outcome, TransferOutcome::Completed) << (finished.failure ? finished.failure->message : "");
        EXPECT_TRUE(HoldsPayload(PartialPath(id), server_->Size()));
    }
}

TEST_F(EngineTest, WebPageInsteadOfFileAndMissingFileFail)
{
    const auto page = Run(Request("/page.bin", 1));
    ASSERT_EQ(page.outcome, TransferOutcome::Failed);
    EXPECT_EQ(page.failure->reason, FailureReason::HtmlInsteadOfFile);

    const auto missing = Run(Request("/missing.bin", 1, 2));
    ASSERT_EQ(missing.outcome, TransferOutcome::Failed);
    EXPECT_EQ(missing.failure->reason, FailureReason::NotFound);
}

TEST_F(EngineTest, EmptyFileCompletes)
{
    const auto finished = Run(Request("/empty.bin"));
    ASSERT_EQ(finished.outcome, TransferOutcome::Completed);
    EXPECT_EQ(finished.totalSize, 0);
    EXPECT_EQ(std::filesystem::file_size(PartialPath()), 0u);
}

TEST_F(EngineTest, ContentDispositionExtendedNameIsDecoded)
{
    const auto finished = Run(Request("/disposition.bin"));
    ASSERT_EQ(finished.outcome, TransferOutcome::Completed);
    const auto info = listener_.WaitFor<ResponseInfo>(DownloadId{1}, 1, 1s);
    ASSERT_TRUE(info);
    EXPECT_EQ(info->fileName, "\xE2\x82\xAC rates: Q3.bin");  // sanitizing is the core's job
}

TEST_F(EngineTest, SpeedLimitHoldsTheRate)
{
    auto request = Request("/range.bin");
    constexpr std::int64_t kLimit = 1024 * 1024;
    request.speedLimitBytesPerSecond = kLimit;
    const auto started = std::chrono::steady_clock::now();
    const auto finished = Run(std::move(request));
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    ASSERT_EQ(finished.outcome, TransferOutcome::Completed);
    const double rate = static_cast<double>(server_->Size()) / seconds;
    EXPECT_NEAR(rate, static_cast<double>(kLimit), kLimit * 0.15) << "seconds: " << seconds;
}

TEST_F(EngineTest, SpeedLimitIsSharedFairlyBetweenConnections)
{
    auto request = Request("/range.bin");
    request.speedLimitBytesPerSecond = 1024 * 1024;
    request.maxConnections = 4;
    engine_->Start(std::move(request));
    // Connections finish segments and get new ones, so compare what each one present in two
    // reports about half a second apart gained in between.
    const auto before = listener_.WaitFor<ProgressInfo>(DownloadId{1}, 1, 30s, [](const ProgressInfo& p) {
        return p.receivedBytes > 1024 * 1024 && p.connections.size() >= 3;
    });
    ASSERT_TRUE(before);
    const std::int64_t target = before->receivedBytes + 512 * 1024;
    const auto after = listener_.WaitFor<ProgressInfo>(DownloadId{1}, 1, 30s, [target](const ProgressInfo& p) {
        return p.receivedBytes > target;
    });
    ASSERT_TRUE(after);
    std::int64_t least = std::numeric_limits<std::int64_t>::max();
    std::int64_t most = 0;
    int compared = 0;
    for (const auto& now : after->connections) {
        for (const auto& then : before->connections) {
            if (then.segmentStart == now.segmentStart) {
                const std::int64_t gained = now.received - then.received;
                least = std::min(least, gained);
                most = std::max(most, gained);
                compared += 1;
            }
        }
    }
    ASSERT_GE(compared, 2);
    EXPECT_GE(least * 10, most * 4) << "least " << least << ", most " << most << " over " << compared << " connections";
    engine_->Stop(DownloadId{1}, 1);
    ASSERT_TRUE(listener_.Finished(DownloadId{1}, 1));
}

TEST_F(EngineTest, GlobalSpeedLimitIsSharedFairlyBetweenDownloads)
{
    engine_->SetGlobalSpeedLimit(1024 * 1024);
    for (std::int64_t id = 1; id <= 2; ++id) {
        auto request = Request("/range.bin", 1, id);
        request.maxConnections = 2;
        engine_->Start(std::move(request));
    }
    const auto first = listener_.WaitFor<ProgressInfo>(DownloadId{1}, 1, 30s, [](const ProgressInfo& p) {
        return p.receivedBytes > 1024 * 1024;
    });
    ASSERT_TRUE(first);
    const auto second = listener_.All<ProgressInfo>(DownloadId{2}, 1);
    ASSERT_FALSE(second.empty());
    const std::int64_t other = second.back().receivedBytes;
    EXPECT_GE(other * 10, first->receivedBytes * 5) << "first " << first->receivedBytes << ", second " << other;
    engine_->SetGlobalSpeedLimit(std::nullopt);
    for (std::int64_t id = 1; id <= 2; ++id) {
        engine_->Stop(DownloadId{id}, 1);
        ASSERT_TRUE(listener_.Finished(DownloadId{id}, 1));
    }
}

TEST_F(EngineTest, FinalizeMovesWithoutOverwriting)
{
    const auto finished = Run(Request("/range.bin"));
    ASSERT_EQ(finished.outcome, TransferOutcome::Completed);
    ldm::core::FinalizeRequest finalize{DownloadId{1}, 1, PartialPath(), directory_ / "out" / "range.bin",
                                        "Thu, 01 Oct 2026 00:00:00 GMT"};
    engine_->Finalize(finalize);
    const auto moved = listener_.WaitFor<ldm::core::FinalizeFinished>(DownloadId{1}, 1, 10s);
    ASSERT_TRUE(moved);
    EXPECT_FALSE(moved->failure);
    EXPECT_FALSE(std::filesystem::exists(PartialPath()));
    EXPECT_TRUE(HoldsPayload(directory_ / "out" / "range.bin", server_->Size()));

    // A second file onto the same name must fail rather than replace it.
    { std::ofstream(directory_ / "other.ldmpart") << "x"; }
    finalize.epoch = 2;
    finalize.partialPath = directory_ / "other.ldmpart";
    engine_->Finalize(finalize);
    const auto refused = listener_.WaitFor<ldm::core::FinalizeFinished>(DownloadId{1}, 2, 10s);
    ASSERT_TRUE(refused);
    EXPECT_TRUE(refused->failure);
    EXPECT_TRUE(HoldsPayload(directory_ / "out" / "range.bin", server_->Size()));
}

TEST_F(EngineTest, ShutdownStopsRunningTransfers)
{
    engine_->Start(Request("/range.bin"));
    ASSERT_TRUE(listener_.WaitFor<ProgressInfo>(DownloadId{1}, 1, 30s, [](const ProgressInfo& progress) {
        return progress.receivedBytes > 256 * 1024;
    }));
    engine_.reset();
    const auto finished = listener_.Finished(DownloadId{1}, 1, 1s);
    ASSERT_TRUE(finished);
    EXPECT_EQ(finished->outcome, TransferOutcome::Stopped);
}

// Synchronization (checklist Q-05): a check that asks the server whether its file changed.
TEST_F(EngineTest, SynchronizationLeavesAnUnchangedFileAlone)
{
    const auto first = Run(Request("/versioned.bin", 1, 1));
    ASSERT_EQ(first.outcome, TransferOutcome::Completed) << (first.failure ? first.failure->message : "");
    const auto info = listener_.WaitFor<ResponseInfo>(DownloadId{1}, 1, 1s);
    ASSERT_TRUE(info);
    ASSERT_TRUE(info->validators.etag);
    ASSERT_TRUE(info->validators.lastModified);

    auto byTag = Request("/versioned.bin", 1, 2);
    byTag.skipIfUnchanged = true;
    byTag.validators = info->validators;
    byTag.knownSize = first.totalSize;
    const auto unchanged = Run(std::move(byTag));
    EXPECT_EQ(unchanged.outcome, TransferOutcome::Unchanged) << (unchanged.failure ? unchanged.failure->message : "");
    EXPECT_TRUE(ReadFile(PartialPath(2)).empty()) << "nothing is written";

    auto byDate = Request("/versioned.bin", 1, 3);
    byDate.skipIfUnchanged = true;
    byDate.validators.lastModified = info->validators.lastModified;
    byDate.knownSize = first.totalSize;
    EXPECT_EQ(Run(std::move(byDate)).outcome, TransferOutcome::Unchanged) << "If-Modified-Since without an ETag";

    server_->Bump();
    auto changed = Request("/versioned.bin", 1, 4);
    changed.skipIfUnchanged = true;
    changed.validators = info->validators;
    changed.knownSize = first.totalSize;
    const auto downloaded = Run(std::move(changed));
    ASSERT_EQ(downloaded.outcome, TransferOutcome::Completed) << (downloaded.failure ? downloaded.failure->message : "");
    EXPECT_EQ(static_cast<std::int64_t>(ReadFile(PartialPath(4)).size()), server_->Size());
}

TEST_F(EngineTest, SynchronizationWithoutValidatorsComparesTheSize)
{
    auto same = FtpRequest();
    same.skipIfUnchanged = true;
    same.knownSize = server_->Size();
    const auto unchanged = Run(std::move(same));
    EXPECT_EQ(unchanged.outcome, TransferOutcome::Unchanged) << (unchanged.failure ? unchanged.failure->message : "");

    auto other = FtpRequest();
    other.id = DownloadId{2};
    other.partialPath = PartialPath(2);
    other.skipIfUnchanged = true;
    other.knownSize = server_->Size() - 1;
    const auto downloaded = Run(std::move(other));
    ASSERT_EQ(downloaded.outcome, TransferOutcome::Completed) << (downloaded.failure ? downloaded.failure->message : "");
    EXPECT_TRUE(HoldsPayload(PartialPath(2), server_->Size()));
}

}  // namespace
