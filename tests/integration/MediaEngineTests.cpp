// Stream runs of the engine (M3 design section 6) against the generated HLS fixtures, served over
// HTTP by the fixture server: every piece lands in its own file, decrypted where the playlist
// says so, and a stopped run continues with only the pieces still missing.
#include "FixtureServer.h"
#include "MediaFixtures.h"
#include "RecordingListener.h"

#include "core/MediaDownloads.h"
#include "core/MediaPieces.h"
#include "engine/Aes128Cbc.h"
#include "engine/CurlEngine.h"
#include "media/DashManifest.h"
#include "media/HlsPlaylist.h"
#include "platform/win/ChildProcess.h"
#include "platform/win/FfmpegCommands.h"
#include "platform/win/FfmpegMediaTools.h"
#include "platform/win/XmlLiteReader.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using std::chrono_literals::operator""ms;
using std::chrono_literals::operator""s;
using ldm::core::FailureReason;
using ldm::core::MediaFinished;
using ldm::core::MediaProgress;
using ldm::core::MediaRequest;
using ldm::core::MediaTrackRequest;
using ldm::core::TransferOutcome;
using ldm::model::DownloadId;
using ldm::test::FixtureServer;
using ldm::test::RecordingListener;
namespace fs = std::filesystem;

std::string ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

class MediaEngineTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        if (!FixtureServer::FindNode()) {
            return;
        }
        std::random_device random;
        fixtureRoot_ = new fs::path(fs::temp_directory_path() / ("ldm-media-engine-" + std::to_string(random())));
        fs::create_directories(*fixtureRoot_);
        auto made = ldm::test::MakeMediaFixtures(*fixtureRoot_);
        ASSERT_TRUE(made) << made.GetError().message;
        fixtures_ = new ldm::test::MediaFixturePaths(made.Value());
        ldm::test::FixtureOptions options;
        options.size = 1024 * 1024;
        options.delayMs = 2;
        options.chunkBytes = 16 * 1024;
        options.mediaRoot = *fixtureRoot_;
        server_ = FixtureServer::Launch(options).release();
    }
    static void TearDownTestSuite()
    {
        delete server_;
        server_ = nullptr;
        delete fixtures_;
        fixtures_ = nullptr;
        if (fixtureRoot_ != nullptr) {
            std::error_code ignored;
            fs::remove_all(*fixtureRoot_, ignored);
            delete fixtureRoot_;
            fixtureRoot_ = nullptr;
        }
    }

    void SetUp() override
    {
        if (server_ == nullptr || fixtures_ == nullptr) {
            GTEST_SKIP() << "Node.js is not installed; fixture tests skipped";
        }
        server_->Reset();
        std::random_device random;
        parts_ = fs::temp_directory_path() / ("ldm-media-run-" + std::to_string(random()));
        ldm::engine::EngineOptions options;
        options.flushInterval = 100ms;
        options.tuning.progressInterval = 50ms;
        options.shutdownTimeout = 5s;
        engine_ = std::make_unique<ldm::engine::CurlEngine>(listener_, options);
    }

    void TearDown() override
    {
        engine_.reset();
        std::error_code ignored;
        fs::remove_all(parts_, ignored);
    }

    // The fixture playlist at `local`, read as if LDM had fetched it from the fixture server.
    static ldm::model::MediaTrack Track(const fs::path& local)
    {
        const std::string relative = fs::relative(local, *fixtureRoot_).generic_string();
        const auto parsed = ldm::media::ParseHls(ReadFile(local), server_->Url("/media/" + relative));
        EXPECT_TRUE(parsed) << (parsed ? "" : parsed.GetError().message);
        ldm::model::MediaTrack track;
        if (parsed) {
            ldm::core::FillTrack(track, std::get<ldm::media::TrackSegments>(parsed.Value()));
        }
        return track;
    }

    MediaTrackRequest TrackRequest(const ldm::model::MediaTrack& track, const std::set<std::uint32_t>& skip = {}) const
    {
        MediaTrackRequest request;
        request.track = 0;
        request.keys = track.keys;
        for (std::uint32_t i = 0; i < track.pieces.size(); ++i) {
            if (skip.contains(i)) {
                continue;
            }
            const ldm::model::MediaPiece& piece = track.pieces[i];
            ldm::core::MediaPieceRequest pieceRequest;
            pieceRequest.index = i;
            pieceRequest.url = piece.url;
            pieceRequest.rangeOffset = piece.rangeOffset;
            pieceRequest.rangeLength = piece.rangeLength;
            pieceRequest.key = piece.key;
            pieceRequest.iv = piece.iv.value_or(ldm::model::AesKey{});
            pieceRequest.path = ldm::core::PiecePath(parts_, 0, track, i);
            request.pieces.push_back(std::move(pieceRequest));
        }
        return request;
    }

    MediaRequest Request(std::vector<MediaTrackRequest> tracks, std::int64_t epoch = 1) const
    {
        MediaRequest request;
        request.id = DownloadId{1};
        request.epoch = epoch;
        request.userAgent = "LDM test";
        request.tracks = std::move(tracks);
        request.maxConnections = 4;
        return request;
    }

    std::optional<MediaFinished> Finished(std::int64_t epoch, std::chrono::milliseconds timeout = 60s)
    {
        return listener_.WaitFor<MediaFinished>(DownloadId{1}, epoch, timeout);
    }

    // Every piece the run reported done, by index.
    std::set<std::uint32_t> Done(std::int64_t epoch)
    {
        std::set<std::uint32_t> done;
        const auto add = [&done](const std::vector<ldm::core::MediaPieceDone>& list) {
            for (const auto& piece : list) {
                EXPECT_TRUE(done.insert(piece.index).second) << "piece " << piece.index << " reported twice";
            }
        };
        for (const auto& progress : listener_.All<MediaProgress>(DownloadId{1}, epoch)) {
            add(progress.done);
        }
        for (const auto& finished : listener_.All<MediaFinished>(DownloadId{1}, epoch)) {
            add(finished.done);
        }
        return done;
    }

    // Waits until the run reported `count` pieces done.
    bool WaitForDone(std::int64_t epoch, size_t count, std::chrono::milliseconds timeout = 30s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (Done(epoch).size() < count) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(20ms);
        }
        return true;
    }

    // Each piece's file holds exactly the bytes its address (and range) serves.
    void ExpectPiecesMatch(const ldm::model::MediaTrack& track, const fs::path& folder)
    {
        for (std::uint32_t i = 0; i < track.pieces.size(); ++i) {
            const auto& piece = track.pieces[i];
            const std::string name = piece.url.substr(piece.url.find_last_of('/') + 1);
            std::string expected = ReadFile(folder / name);
            if (piece.rangeOffset) {
                expected = expected.substr(static_cast<size_t>(*piece.rangeOffset), static_cast<size_t>(*piece.rangeLength));
            }
            EXPECT_EQ(ReadFile(ldm::core::PiecePath(parts_, 0, track, i)), expected) << "piece " << i;
        }
    }

    static fs::path* fixtureRoot_;
    static ldm::test::MediaFixturePaths* fixtures_;
    static FixtureServer* server_;
    RecordingListener listener_;
    std::unique_ptr<ldm::engine::CurlEngine> engine_;
    fs::path parts_;
};

fs::path* MediaEngineTest::fixtureRoot_ = nullptr;
ldm::test::MediaFixturePaths* MediaEngineTest::fixtures_ = nullptr;
FixtureServer* MediaEngineTest::server_ = nullptr;

TEST_F(MediaEngineTest, TransportStreamPiecesMatchTheServedSegments)
{
    const auto track = Track(fixtures_->hlsTs);
    ASSERT_EQ(track.pieces.size(), 4u);
    engine_->StartMedia(Request({TrackRequest(track)}));
    const auto finished = Finished(1);
    ASSERT_TRUE(finished);
    ASSERT_EQ(finished->outcome, TransferOutcome::Completed) << (finished->failure ? finished->failure->message : "");
    EXPECT_EQ(Done(1), (std::set<std::uint32_t>{0, 1, 2, 3}));
    ExpectPiecesMatch(track, fixtures_->hlsTs.parent_path());
    EXPECT_GE(server_->Log().value("maxActive", 0), 2) << "pieces are fetched several at once";
}

TEST_F(MediaEngineTest, FragmentedMp4InitSectionIsAPieceOfItsOwn)
{
    const auto track = Track(fixtures_->hlsFmp4);
    ASSERT_GE(track.pieces.size(), 2u);
    EXPECT_EQ(track.pieces[0].kind, ldm::model::MediaPiece::Kind::Init);
    engine_->StartMedia(Request({TrackRequest(track)}));
    const auto finished = Finished(1);
    ASSERT_TRUE(finished);
    ASSERT_EQ(finished->outcome, TransferOutcome::Completed) << (finished->failure ? finished->failure->message : "");
    ExpectPiecesMatch(track, fixtures_->hlsFmp4.parent_path());
}

TEST_F(MediaEngineTest, ByteRangePiecesAreTheirPartsOfTheFile)
{
    const auto track = Track(fixtures_->hlsByteRange);
    ASSERT_GE(track.pieces.size(), 4u);
    ASSERT_TRUE(track.pieces[1].rangeOffset.has_value());
    engine_->StartMedia(Request({TrackRequest(track)}));
    const auto finished = Finished(1);
    ASSERT_TRUE(finished);
    ASSERT_EQ(finished->outcome, TransferOutcome::Completed) << (finished->failure ? finished->failure->message : "");
    ExpectPiecesMatch(track, fixtures_->hlsByteRange.parent_path());
    const nlohmann::json log = server_->Log();  // a temporary in the range-for would dangle
    for (const auto& entry : log.at("entries")) {
        if (entry.value("event", "") == "request" && entry.value("path", "").find("/media/hls-byterange/") == 0) {
            EXPECT_FALSE(entry.at("range").is_null()) << "every piece asks for its range";
        }
    }
}

TEST_F(MediaEngineTest, EncryptedPiecesAreStoredDecrypted)
{
    const auto track = Track(fixtures_->hlsAes);
    ASSERT_EQ(track.keys.size(), 1u);
    engine_->StartMedia(Request({TrackRequest(track)}));
    const auto finished = Finished(1);
    ASSERT_TRUE(finished);
    ASSERT_EQ(finished->outcome, TransferOutcome::Completed) << (finished->failure ? finished->failure->message : "");

    const std::string keyBytes = ReadFile(fixtures_->hlsAesKey);
    ASSERT_EQ(keyBytes.size(), 16u);
    ldm::model::AesKey key{};
    std::copy(keyBytes.begin(), keyBytes.end(), key.begin());
    for (std::uint32_t i = 0; i < track.pieces.size(); ++i) {
        const auto& piece = track.pieces[i];
        const std::string cipher = ReadFile(fixtures_->hlsAes.parent_path() / piece.url.substr(piece.url.find_last_of('/') + 1));
        auto decryptor = ldm::engine::Aes128CbcDecryptor::Create(key, piece.iv.value());
        ASSERT_TRUE(decryptor);
        std::vector<std::byte> plain;
        const auto* bytes = reinterpret_cast<const std::byte*>(cipher.data());
        ASSERT_TRUE(decryptor->Update(std::span<const std::byte>(bytes, cipher.size()), plain));
        ASSERT_TRUE(decryptor->Finish(plain));
        const std::string stored = ReadFile(ldm::core::PiecePath(parts_, 0, track, i));
        ASSERT_EQ(stored.size(), plain.size()) << "piece " << i;
        EXPECT_TRUE(std::equal(stored.begin(), stored.end(), plain.begin(),
                               [](char a, std::byte b) { return static_cast<std::byte>(a) == b; }))
            << "piece " << i;
        EXPECT_EQ(static_cast<unsigned char>(stored.front()), 0x47u) << "an MPEG-TS packet starts each piece";
    }
}

TEST_F(MediaEngineTest, AStoppedRunContinuesWithOnlyTheMissingPieces)
{
    const auto track = Track(fixtures_->hlsTs);
    MediaRequest first = Request({TrackRequest(track)});
    first.maxConnections = 1;
    first.speedLimitBytesPerSecond = 64 * 1024;  // slow enough to stop between pieces
    engine_->StartMedia(first);
    ASSERT_TRUE(listener_.WaitFor<MediaProgress>(DownloadId{1}, 1, 30s,
                                                 [](const MediaProgress& p) { return !p.done.empty(); }));
    engine_->Stop(DownloadId{1}, 1);
    const auto stopped = Finished(1);
    ASSERT_TRUE(stopped);
    ASSERT_EQ(stopped->outcome, TransferOutcome::Stopped);
    const std::set<std::uint32_t> done = Done(1);
    ASSERT_FALSE(done.empty());
    ASSERT_LT(done.size(), track.pieces.size());

    server_->Reset();
    engine_->StartMedia(Request({TrackRequest(track, done)}, 2));
    const auto completed = Finished(2);
    ASSERT_TRUE(completed);
    ASSERT_EQ(completed->outcome, TransferOutcome::Completed);
    std::set<std::uint32_t> all = done;
    for (const auto index : Done(2)) {
        EXPECT_TRUE(all.insert(index).second) << "piece " << index << " fetched again";
    }
    EXPECT_EQ(all.size(), track.pieces.size());
    ExpectPiecesMatch(track, fixtures_->hlsTs.parent_path());
}

TEST_F(MediaEngineTest, ALiveRunWaitsForMorePiecesUntilTheRecordingEnds)
{
    const auto track = Track(fixtures_->hlsTs);
    ASSERT_EQ(track.pieces.size(), 4u);
    MediaTrackRequest first = TrackRequest(track, {2, 3});
    first.live = true;
    engine_->StartMedia(Request({first}));
    ASSERT_TRUE(WaitForDone(1, 2));
    EXPECT_FALSE(Finished(1, 300ms)) << "a live run waits for more once its pieces are done";

    const MediaTrackRequest rest = TrackRequest(track, {0, 1});
    ldm::core::MediaTrackExtension added;
    added.track = 0;
    added.pieces = rest.pieces;
    added.live = false;  // the playlist said the stream ended
    engine_->ExtendMedia(DownloadId{1}, 1, ldm::core::MediaExtension{{added}});
    const auto finished = Finished(1);
    ASSERT_TRUE(finished);
    ASSERT_EQ(finished->outcome, TransferOutcome::Completed) << (finished->failure ? finished->failure->message : "");
    EXPECT_EQ(Done(1), (std::set<std::uint32_t>{0, 1, 2, 3}));
    ExpectPiecesMatch(track, fixtures_->hlsTs.parent_path());
}

TEST_F(MediaEngineTest, ALiveRunWaitingForPiecesStopsAtOnce)
{
    const auto track = Track(fixtures_->hlsTs);
    MediaTrackRequest live = TrackRequest(track);
    live.live = true;
    engine_->StartMedia(Request({live}));
    ASSERT_TRUE(WaitForDone(1, track.pieces.size()));
    // An extension for another epoch, or one that adds nothing, does not end it.
    engine_->ExtendMedia(DownloadId{1}, 7, ldm::core::MediaExtension{{ldm::core::MediaTrackExtension{0, {}, {}, false}}});
    engine_->ExtendMedia(DownloadId{1}, 1, ldm::core::MediaExtension{{ldm::core::MediaTrackExtension{0, {}, {}, true}}});
    EXPECT_FALSE(Finished(1, 300ms));
    EXPECT_FALSE(listener_.All<MediaProgress>(DownloadId{1}, 1).empty()) << "an idle run still reports";

    const auto stopAt = std::chrono::steady_clock::now();
    engine_->Stop(DownloadId{1}, 1);
    const auto stopped = Finished(1, 5s);
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped->outcome, TransferOutcome::Stopped);
    EXPECT_LT(std::chrono::steady_clock::now() - stopAt, 2s);
}

TEST_F(MediaEngineTest, LoopbackAddressesAreRefusedForAStreamFromThePublicInternet)
{
    const auto track = Track(fixtures_->hlsTs);
    MediaRequest request = Request({TrackRequest(track)});
    request.refusePrivateAddresses = true;
    engine_->StartMedia(request);
    const auto finished = Finished(1);
    ASSERT_TRUE(finished);
    EXPECT_EQ(finished->outcome, TransferOutcome::Failed);
    ASSERT_TRUE(finished->failure);
    EXPECT_EQ(finished->failure->reason, FailureReason::PrivateAddress);
    EXPECT_TRUE(Done(1).empty());
    const nlohmann::json log = server_->Log();
    for (const auto& entry : log.at("entries")) {
        const bool control = entry.value("path", "").starts_with("/control/");  // the log request itself
        EXPECT_TRUE(control || entry.value("event", "") != "request") << "nothing reached the server: " << entry.dump();
    }
}

TEST_F(MediaEngineTest, ASitesOwnHeadersGoWithEveryPieceAndKey)
{
    const auto track = Track(fixtures_->hlsAes);
    MediaRequest request = Request({TrackRequest(track)});
    request.headers = {"X-Stream-Key: k1"};
    engine_->StartMedia(std::move(request));
    const auto finished = Finished(1);
    ASSERT_TRUE(finished);
    ASSERT_EQ(finished->outcome, TransferOutcome::Completed) << (finished->failure ? finished->failure->message : "");
    const nlohmann::json log = server_->Log();
    int requests = 0;
    for (const auto& entry : log.at("entries")) {
        if (entry.value("event", "") == "request" && entry.value("path", "").starts_with("/media/")) {
            EXPECT_EQ(entry.value("streamKey", std::string()), "k1") << entry.dump();
            ++requests;
        }
    }
    EXPECT_GT(requests, static_cast<int>(track.pieces.size())) << "the pieces and the key";
}

TEST_F(MediaEngineTest, ASitesOwnHeadersStayOffAPieceRedirectedToAnotherHost)
{
    const auto track = Track(fixtures_->hlsTs);
    auto redirected = track;
    const std::string from = "127.0.0.1";
    std::string other = track.pieces[1].url;
    other.replace(other.find(from), from.size(), "localhost");
    redirected.pieces[1].url = server_->Url("/redirect-to?to=" + other);
    MediaRequest request = Request({TrackRequest(redirected)});
    request.headers = {"X-Stream-Key: k1"};
    engine_->StartMedia(std::move(request));
    const auto finished = Finished(1);
    ASSERT_TRUE(finished);
    ASSERT_EQ(finished->outcome, TransferOutcome::Completed) << (finished->failure ? finished->failure->message : "");
    const std::string served = ReadFile(fixtures_->hlsTs.parent_path() / other.substr(other.find_last_of('/') + 1));
    EXPECT_EQ(ReadFile(ldm::core::PiecePath(parts_, 0, redirected, 1)), served);
    const nlohmann::json log = server_->Log();
    int elsewhere = 0;
    for (const auto& entry : log.at("entries")) {
        if (entry.value("event", "") != "request" || !entry.value("path", "").starts_with("/media")) {
            continue;
        }
        if (entry.value("host", "").starts_with("localhost:")) {
            EXPECT_TRUE(entry.at("streamKey").is_null()) << "the key reached the other host: " << entry.dump();
            ++elsewhere;
        } else {
            EXPECT_EQ(entry.value("streamKey", std::string()), "k1") << entry.dump();
        }
    }
    EXPECT_EQ(elsewhere, 1);
}

TEST_F(MediaEngineTest, AMissingPieceFailsTheRun)
{
    auto track = Track(fixtures_->hlsTs);
    track.pieces[2].url = server_->Url("/media-gone/seg002.ts");
    engine_->StartMedia(Request({TrackRequest(track)}));
    const auto finished = Finished(1);
    ASSERT_TRUE(finished);
    EXPECT_EQ(finished->outcome, TransferOutcome::Failed);
    ASSERT_TRUE(finished->failure);
    EXPECT_EQ(finished->failure->reason, FailureReason::NotFound);
}

TEST_F(MediaEngineTest, AWholeFileTrackIsDownloadedInRanges)
{
    MediaTrackRequest whole;
    whole.track = 1;
    whole.wholeFile = true;
    whole.url = server_->Url("/range.bin");
    whole.path = parts_ / "t1.mp4";
    engine_->StartMedia(Request({TrackRequest(Track(fixtures_->hlsTs)), whole}));
    const auto finished = Finished(1);
    ASSERT_TRUE(finished);
    ASSERT_EQ(finished->outcome, TransferOutcome::Completed) << (finished->failure ? finished->failure->message : "");
    ASSERT_EQ(finished->ranges.size(), 1u);
    EXPECT_EQ(finished->ranges[0].track, 1u);
    EXPECT_EQ(finished->ranges[0].totalSize, server_->Size());
    const std::string file = ReadFile(whole.path);
    ASSERT_EQ(static_cast<std::int64_t>(file.size()), server_->Size());
    for (size_t i = 0; i < file.size(); i += 4099) {
        ASSERT_EQ(static_cast<std::uint8_t>(file[i]), ldm::test::FixtureByte(static_cast<std::int64_t>(i))) << i;
    }
}

TEST_F(MediaEngineTest, AResumedWholeFileTrackCountsOnlyTheBytesOfThisRun)
{
    // Half of the file is on disk from an earlier run; the coordinator adds that half itself.
    const std::int64_t size = server_->Size();
    const std::int64_t half = size / 2;
    MediaTrackRequest whole;
    whole.track = 0;
    whole.wholeFile = true;
    whole.url = server_->Url("/range.bin");
    whole.path = parts_ / "t0.mp4";
    whole.ranges = {ldm::model::Segment{0, size, half}};
    fs::create_directories(parts_);
    {
        std::ofstream out(whole.path, std::ios::binary);
        ASSERT_TRUE(out) << whole.path;
        for (std::int64_t i = 0; i < half; ++i) {
            out.put(static_cast<char>(ldm::test::FixtureByte(i)));
        }
    }
    engine_->StartMedia(Request({whole}));
    const auto finished = Finished(1);
    ASSERT_TRUE(finished);
    ASSERT_EQ(finished->outcome, TransferOutcome::Completed) << (finished->failure ? finished->failure->message : "");
    const auto reports = listener_.All<ldm::core::MediaProgress>(DownloadId{1}, 1);
    ASSERT_FALSE(reports.empty());
    for (const auto& report : reports) {
        EXPECT_GE(report.receivedBytes, 0);
        EXPECT_LE(report.receivedBytes, size - half) << "the half on disk is not received again";
    }
    const std::string file = ReadFile(whole.path);
    ASSERT_EQ(static_cast<std::int64_t>(file.size()), size);
    for (size_t i = 0; i < file.size(); i += 4099) {
        ASSERT_EQ(static_cast<std::uint8_t>(file[i]), ldm::test::FixtureByte(static_cast<std::int64_t>(i))) << i;
    }
}

TEST_F(MediaEngineTest, FetchesSayWhetherTheyCameFromAPrivateNetwork)
{
    ldm::core::FetchRequest fetch;
    fetch.fetchId = 7;
    fetch.url = server_->Url("/media/hls-ts/index.m3u8");
    fetch.userAgent = "LDM test";
    fetch.maxBytes = 1024 * 1024;
    engine_->Fetch(fetch);
    fetch.fetchId = 8;
    fetch.refusePrivateAddresses = true;
    engine_->Fetch(fetch);
    const auto answer = [this](std::int64_t id) {
        return listener_.WaitFor<ldm::core::FetchFinished>(DownloadId{}, 0, 30s, [id](const ldm::core::FetchFinished& f) {
            return f.fetchId == id;
        });
    };
    const auto allowed = answer(7);
    ASSERT_TRUE(allowed);
    EXPECT_FALSE(allowed->failure);
    EXPECT_TRUE(allowed->fromPrivateNetwork);
    const auto refused = answer(8);
    ASSERT_TRUE(refused);
    ASSERT_TRUE(refused->failure);
    EXPECT_EQ(refused->failure->reason, FailureReason::PrivateAddress);
}


// ---- joining (FfmpegMediaTools) ----

struct Probed {
    double seconds = 0;
    int video = 0;
    int audio = 0;
    int subtitles = 0;
};

Probed Probe(const fs::path& file)
{
    ldm::platform::ChildProcessRequest request;
    request.program = ldm::test::FfprobeExe();
    request.arguments = ldm::platform::ProbeArguments(file);
    const auto run = ldm::platform::RunChildProcess(request);
    EXPECT_TRUE(run && run.Value().exitCode == 0) << (run ? run.Value().output : run.GetError().message);
    Probed probed;
    if (!run) {
        return probed;
    }
    const auto json = nlohmann::json::parse(run.Value().output, nullptr, false);
    if (json.is_discarded()) {
        ADD_FAILURE() << run.Value().output;
        return probed;
    }
    probed.seconds = std::stod(json.at("format").value("duration", std::string("0")));
    for (const auto& stream : json.value("streams", nlohmann::json::array())) {
        const std::string type = stream.value("codec_type", std::string());
        probed.video += type == "video" ? 1 : 0;
        probed.audio += type == "audio" ? 1 : 0;
        probed.subtitles += type == "subtitle" ? 1 : 0;
    }
    return probed;
}

class MediaJoinTest : public MediaEngineTest {
protected:
    void SetUp() override
    {
        MediaEngineTest::SetUp();
        tools_ = std::make_unique<ldm::platform::FfmpegMediaTools>(ldm::test::FfmpegExe().parent_path(), listener_);
    }
    void TearDown() override
    {
        tools_.reset();
        MediaEngineTest::TearDown();
    }

    // The address the fixture server serves `local` at.
    static std::string UrlOf(const fs::path& local)
    {
        return server_->Url("/media/" + fs::relative(local, *fixtureRoot_).generic_string());
    }
    static fs::path LocalOf(const std::string& url)
    {
        const std::string prefix = server_->Url("/media/");
        EXPECT_TRUE(url.starts_with(prefix)) << url;
        return *fixtureRoot_ / fs::path(url.substr(std::min(url.size(), prefix.size())));
    }
    static ldm::model::MediaTrack HlsTrack(const fs::path& local, ldm::model::TrackRole role)
    {
        ldm::model::MediaTrack track = Track(local);
        track.role = role;
        track.sourceUrl = UrlOf(local);
        return track;
    }

    // Downloads every track of `download` through the engine, then joins them.
    std::optional<ldm::core::MuxFinished> DownloadAndJoin(ldm::model::Download& download, std::int64_t epoch = 1)
    {
        download.id = DownloadId{1};
        download.epoch = epoch;
        download.mediaFromPrivateNetwork = true;
        MediaRequest request = ldm::core::BuildMediaRequest(download, parts_);
        request.userAgent = "LDM test";
        request.maxConnections = 4;
        engine_->StartMedia(request);
        const auto finished = Finished(epoch);
        if (!finished || finished->outcome != TransferOutcome::Completed) {
            ADD_FAILURE() << "the download did not complete: "
                          << (finished && finished->failure ? finished->failure->message : "no answer");
            return std::nullopt;
        }
        tools_->Mux(ldm::core::BuildMuxRequest(download, parts_));
        auto muxed = listener_.WaitFor<ldm::core::MuxFinished>(DownloadId{1}, epoch, 120s);
        EXPECT_TRUE(muxed) << "the join did not answer";
        if (muxed && muxed->failure) {
            ADD_FAILURE() << muxed->failure->message;
        }
        return muxed;
    }

    std::unique_ptr<ldm::platform::FfmpegMediaTools> tools_;
};

TEST_F(MediaJoinTest, TransportStreamBecomesAnMp4WithItsLengthAndStreams)
{
    ldm::model::Download download;
    download.kind = ldm::model::DownloadKind::Hls;
    download.tracks = {HlsTrack(fixtures_->hlsTs, ldm::model::TrackRole::Video)};
    const auto muxed = DownloadAndJoin(download);
    ASSERT_TRUE(muxed && !muxed->failure);
    EXPECT_EQ(muxed->extension, "mp4");
    EXPECT_EQ(muxed->output, parts_ / L"output.mp4");
    EXPECT_EQ(muxed->bytes, static_cast<std::int64_t>(fs::file_size(muxed->output)));
    const Probed probed = Probe(muxed->output);
    EXPECT_NEAR(probed.seconds, ldm::test::kFixtureSeconds, 0.3);
    EXPECT_EQ(probed.video, 1);
    EXPECT_EQ(probed.audio, 1);
    EXPECT_NE(muxed->mediaInfo.find("320x240, 25 fps, MPEG-4; AAC, stereo, 48 kHz"), std::string::npos) << muxed->mediaInfo;
}

TEST_F(MediaJoinTest, FragmentedMp4ByteRangeAndEncryptedStreamsJoin)
{
    std::int64_t epoch = 0;
    for (const fs::path* fixture : {&fixtures_->hlsFmp4, &fixtures_->hlsByteRange, &fixtures_->hlsAes}) {
        SCOPED_TRACE(fixture->string());
        std::error_code ignored;
        fs::remove_all(parts_, ignored);  // each run its own epoch and its own pieces
        ldm::model::Download download;
        download.kind = ldm::model::DownloadKind::Hls;
        download.tracks = {HlsTrack(*fixture, ldm::model::TrackRole::Video)};
        const auto muxed = DownloadAndJoin(download, ++epoch);
        ASSERT_TRUE(muxed && !muxed->failure);
        const Probed probed = Probe(muxed->output);
        EXPECT_NEAR(probed.seconds, ldm::test::kFixtureSeconds, 0.3);
        EXPECT_EQ(probed.video, 1);
        EXPECT_EQ(probed.audio, 1);
    }
}

TEST_F(MediaJoinTest, SeparateVideoAndAudioTracksBecomeOneFile)
{
    const std::string text = ReadFile(fixtures_->hlsMaster);
    const auto master = ldm::media::ParseHls(text, UrlOf(fixtures_->hlsMaster));
    ASSERT_TRUE(master);
    const auto& offer = std::get<ldm::media::MediaOffer>(master.Value());
    const auto& video = std::get<ldm::media::PlaylistAddress>(offer.variants.at(0).source);
    const auto& audio = std::get<ldm::media::PlaylistAddress>(*offer.audio.at(0).source);
    ldm::model::Download download;
    download.kind = ldm::model::DownloadKind::Hls;
    download.tracks = {HlsTrack(LocalOf(video.url), ldm::model::TrackRole::Video),
                       HlsTrack(LocalOf(audio.url), ldm::model::TrackRole::Audio)};
    const auto muxed = DownloadAndJoin(download);
    ASSERT_TRUE(muxed && !muxed->failure);
    const Probed probed = Probe(muxed->output);
    EXPECT_NEAR(probed.seconds, ldm::test::kFixtureSeconds, 0.3);
    EXPECT_EQ(probed.video, 1);
    EXPECT_EQ(probed.audio, 1);
    EXPECT_EQ(probed.subtitles, 0) << "subtitles are downloads of their own";
}

TEST_F(MediaJoinTest, HlsSubtitlesBecomeAWebVttFile)
{
    const auto master = ldm::media::ParseHls(ReadFile(fixtures_->hlsMaster), UrlOf(fixtures_->hlsMaster));
    ASSERT_TRUE(master);
    const auto& offer = std::get<ldm::media::MediaOffer>(master.Value());
    const auto& subtitles = std::get<ldm::media::PlaylistAddress>(*offer.subtitles.at(0).source);
    ldm::model::Download download;
    download.kind = ldm::model::DownloadKind::Hls;
    download.tracks = {HlsTrack(LocalOf(subtitles.url), ldm::model::TrackRole::Subtitles)};
    download.tracks[0].language = "en";
    const auto muxed = DownloadAndJoin(download);
    ASSERT_TRUE(muxed && !muxed->failure);
    EXPECT_EQ(muxed->extension, "vtt");
    const std::string vtt = ReadFile(muxed->output);
    EXPECT_TRUE(vtt.starts_with("WEBVTT")) << vtt.substr(0, 40);
    EXPECT_NE(vtt.find("First cue"), std::string::npos);
    EXPECT_NE(vtt.find("Second cue"), std::string::npos);
    EXPECT_EQ(vtt.find("WEBVTT", 1), std::string::npos) << "the pieces' headers stay out of the cues:\n" << vtt;
    EXPECT_EQ(muxed->mediaInfo, "WebVTT subtitles, en");
}

TEST_F(MediaJoinTest, DashVideoAndAudioBecomeOneFile)
{
    auto root = ldm::platform::ReadXml(ReadFile(fixtures_->dash));
    ASSERT_TRUE(root);
    auto offer = ldm::media::ParseMpd(root.Value(), UrlOf(fixtures_->dash), std::chrono::system_clock::now());
    ASSERT_TRUE(offer) << offer.GetError().message;
    ldm::model::Download download;
    download.kind = ldm::model::DownloadKind::Dash;
    ldm::model::MediaTrack video;
    video.role = ldm::model::TrackRole::Video;
    ldm::core::FillTrack(video, std::get<ldm::media::TrackSegments>(offer.Value().variants.at(0).source));
    ldm::model::MediaTrack audio;
    audio.role = ldm::model::TrackRole::Audio;
    ldm::core::FillTrack(audio, std::get<ldm::media::TrackSegments>(*offer.Value().audio.at(0).source));
    download.tracks = {video, audio};
    const auto muxed = DownloadAndJoin(download);
    ASSERT_TRUE(muxed && !muxed->failure);
    const Probed probed = Probe(muxed->output);
    EXPECT_NEAR(probed.seconds, ldm::test::kFixtureSeconds, 0.3);
    EXPECT_EQ(probed.video, 1);
    EXPECT_EQ(probed.audio, 1);
}

// The core's part of a live recording, played by the test: the playlist read again every half
// second while the engine records, until the stream ends; then the recording is joined.
TEST_F(MediaJoinTest, ALiveStreamIsRecordedUntilItEndsAndJoined)
{
    const std::string relative = fs::relative(fixtures_->hlsTs, *fixtureRoot_).generic_string();
    const std::string live = server_->Url("/media-live/" + relative);
    std::int64_t nextFetch = 100;
    const auto fetch = [&]() -> std::optional<ldm::core::FetchFinished> {
        ldm::core::FetchRequest request;
        request.fetchId = ++nextFetch;
        request.url = live;
        request.userAgent = "LDM test";
        request.maxBytes = 1024 * 1024;
        engine_->Fetch(request);
        const std::int64_t id = request.fetchId;
        return listener_.WaitFor<ldm::core::FetchFinished>(DownloadId{}, 0, 30s, [id](const ldm::core::FetchFinished& f) {
            return f.fetchId == id;
        });
    };

    ldm::model::Download download;
    download.id = DownloadId{1};
    download.epoch = 1;
    download.kind = ldm::model::DownloadKind::Hls;
    download.mediaFromPrivateNetwork = true;
    download.tracks.resize(1);
    download.tracks[0].sourceUrl = live;
    const auto first = fetch();
    ASSERT_TRUE(first && !first->failure);
    ASSERT_FALSE(ldm::core::ListTrack(download.tracks[0], *first));
    ASSERT_TRUE(download.tracks[0].live);
    ASSERT_EQ(download.tracks[0].pieces.size(), 3u) << "the window";

    MediaRequest request = ldm::core::BuildMediaRequest(download, parts_);
    request.userAgent = "LDM test";
    request.maxConnections = 4;
    engine_->StartMedia(request);
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (download.tracks[0].live && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(500ms);
        const auto reloaded = fetch();
        ASSERT_TRUE(reloaded && !reloaded->failure);
        const auto reload = ldm::core::ReloadTrack(download.tracks[0], *reloaded);
        ASSERT_FALSE(reload.failure) << reload.failure->message;
        if (reload.append.changed) {
            engine_->ExtendMedia(DownloadId{1}, 1,
                                 ldm::core::MediaExtension{{ldm::core::BuildTrackExtension(download, parts_, 0, reload.append)}});
        }
    }
    ASSERT_FALSE(download.tracks[0].live) << "the stream did not end";
    ASSERT_EQ(download.tracks[0].pieces.size(), 4u);
    const auto finished = Finished(1);
    ASSERT_TRUE(finished);
    ASSERT_EQ(finished->outcome, TransferOutcome::Completed) << (finished->failure ? finished->failure->message : "");
    for (std::uint32_t index : Done(1)) {
        download.tracks[0].pieces.at(index).done = true;
    }

    tools_->Mux(ldm::core::BuildMuxRequest(download, parts_));
    const auto muxed = listener_.WaitFor<ldm::core::MuxFinished>(DownloadId{1}, 1, 120s);
    ASSERT_TRUE(muxed && !muxed->failure) << (muxed && muxed->failure ? muxed->failure->message : "no answer");
    const Probed probed = Probe(muxed->output);
    EXPECT_NEAR(probed.seconds, ldm::test::kFixtureSeconds, 0.3);
    EXPECT_EQ(probed.video, 1);
}

TEST_F(MediaJoinTest, AFinishedMediaFileIsDescribed)
{
    ldm::model::Download download;
    download.kind = ldm::model::DownloadKind::Hls;
    download.tracks = {HlsTrack(fixtures_->hlsTs, ldm::model::TrackRole::Video)};
    const auto muxed = DownloadAndJoin(download);
    ASSERT_TRUE(muxed && !muxed->failure);
    // The joined MP4 stands in for a plain MP4 download; its name does not matter to ffprobe.
    const fs::path file = parts_ / L"clip.ldmpart";
    fs::rename(muxed->output, file);
    tools_->Describe(ldm::core::DescribeRequest{DownloadId{1}, 5, file});
    const auto described = listener_.WaitFor<ldm::core::DescribeFinished>(DownloadId{1}, 5, 60s);
    ASSERT_TRUE(described);
    EXPECT_FALSE(described->stopped);
    EXPECT_NE(described->mediaInfo.find("320x240, 25 fps, MPEG-4; AAC, stereo, 48 kHz"), std::string::npos)
        << described->mediaInfo;

    tools_->Describe(ldm::core::DescribeRequest{DownloadId{1}, 6, parts_ / L"missing.mp4"});
    const auto missing = listener_.WaitFor<ldm::core::DescribeFinished>(DownloadId{1}, 6, 60s);
    ASSERT_TRUE(missing);
    EXPECT_TRUE(missing->mediaInfo.empty()) << "nothing to say is not a failure";
}

TEST_F(MediaJoinTest, ACancelledJoinAnswersStopped)
{
    ldm::core::MuxRequest request;
    request.id = DownloadId{1};
    request.epoch = 9;
    request.folder = parts_;
    request.preferredExtension = "mp4";
    // Queued behind nothing, cancelled before or while it runs: either way it answers once.
    tools_->Mux(request);
    tools_->Cancel(DownloadId{1}, 9);
    const auto answer = listener_.WaitFor<ldm::core::MuxFinished>(DownloadId{1}, 9, 30s);
    ASSERT_TRUE(answer);
    EXPECT_TRUE(answer->stopped || answer->failure) << "a request without tracks cannot succeed";
}

}  // namespace
