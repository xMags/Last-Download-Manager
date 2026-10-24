// Stream runs of the engine (M3 design section 6) against the generated HLS fixtures, served over
// HTTP by the fixture server: every piece lands in its own file, decrypted where the playlist
// says so, and a stopped run continues with only the pieces still missing.
#include "FixtureServer.h"
#include "MediaFixtures.h"
#include "RecordingListener.h"

#include "core/MediaPieces.h"
#include "engine/Aes128Cbc.h"
#include "engine/CurlEngine.h"
#include "media/HlsPlaylist.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <string>
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

}  // namespace
