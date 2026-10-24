#include "core/MediaDownloads.h"
#include "core/MediaPieces.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using ldm::core::AppendTrack;
using ldm::media::TrackSegments;
using ldm::model::MediaPiece;
using ldm::model::MediaTrack;

const std::string kBase = "https://live.test/";

// A live playlist's list: two-second segments `first` to `first + count - 1`.
TrackSegments Listed(std::uint64_t first, int count, bool live = true)
{
    TrackSegments segments;
    segments.live = live;
    segments.targetSeconds = 2;
    for (int i = 0; i < count; ++i) {
        ldm::media::Segment segment;
        segment.sequence = first + static_cast<std::uint64_t>(i);
        segment.url = kBase + "seg" + std::to_string(segment.sequence) + ".ts";
        segment.seconds = 2.0;
        segments.segments.push_back(segment);
    }
    return segments;
}

MediaTrack Recording(std::uint64_t first, int count)
{
    MediaTrack track;
    ldm::core::FillTrack(track, Listed(first, count));
    return track;
}

TEST(AppendTrack, FillingALivePlaylistMakesALiveTrackOfPieces)
{
    MediaTrack track = Recording(10, 1);
    EXPECT_TRUE(track.live);
    EXPECT_FALSE(track.wholeFile) << "one segment now, more later";
    ASSERT_EQ(track.pieces.size(), 1u);

    MediaTrack empty;
    ldm::core::FillTrack(empty, Listed(0, 0));
    EXPECT_TRUE(empty.Listed()) << "a live playlist with nothing yet is read";
}

TEST(AppendTrack, AddsOnlyWhatFollowsTheLastSegment)
{
    MediaTrack track = Recording(10, 3);
    const auto append = AppendTrack(track, Listed(11, 4));
    EXPECT_TRUE(append.changed);
    EXPECT_EQ(append.firstNewPiece, 3u);
    ASSERT_EQ(track.pieces.size(), 5u);
    EXPECT_EQ(track.pieces[3].sequence, 13u);
    EXPECT_EQ(track.pieces[4].url, kBase + "seg14.ts");
    EXPECT_EQ(track.pieces[4].durationUs, 2'000'000);
    EXPECT_FALSE(track.pieces[3].discontinuity);
    EXPECT_TRUE(track.live);
}

TEST(AppendTrack, NothingNewChangesNothing)
{
    MediaTrack track = Recording(10, 3);
    const auto append = AppendTrack(track, Listed(10, 3));
    EXPECT_FALSE(append.changed);
    EXPECT_EQ(track.pieces.size(), 3u);
}

TEST(AppendTrack, MissedSegmentsLeaveADiscontinuity)
{
    MediaTrack track = Recording(10, 3);
    AppendTrack(track, Listed(30, 2));
    ASSERT_EQ(track.pieces.size(), 5u);
    EXPECT_TRUE(track.pieces[3].discontinuity);
    EXPECT_FALSE(track.pieces[4].discontinuity);
}

TEST(AppendTrack, TheEndOfTheStreamEndsTheRecording)
{
    MediaTrack track = Recording(10, 3);
    const auto append = AppendTrack(track, Listed(11, 3, false));
    EXPECT_TRUE(append.changed);
    EXPECT_FALSE(track.live);
    EXPECT_EQ(track.pieces.size(), 4u);

    MediaTrack quiet = Recording(10, 3);
    EXPECT_TRUE(AppendTrack(quiet, Listed(10, 3, false)).changed) << "ending is a change even with no segment";
}

TEST(AppendTrack, NumberingThatStartedOverIsAllNew)
{
    MediaTrack track = Recording(500, 3);
    AppendTrack(track, Listed(0, 2));
    ASSERT_EQ(track.pieces.size(), 5u);
    EXPECT_EQ(track.pieces[3].sequence, 0u);
    EXPECT_TRUE(track.pieces[3].discontinuity);
}

TEST(AppendTrack, AnOlderCopyOfThePlaylistAddsNothing)
{
    MediaTrack track = Recording(10, 5);
    const auto append = AppendTrack(track, Listed(9, 3));  // a cache's copy from before
    EXPECT_FALSE(append.changed);
    EXPECT_EQ(track.pieces.size(), 5u);
}

TEST(AppendTrack, NewInitSectionsAndKeysAreAddedOnceAndReferenced)
{
    MediaTrack track;
    TrackSegments first = Listed(1, 2);
    first.inits.push_back({kBase + "init-a.mp4", std::nullopt});
    first.keys.push_back({kBase + "key-a", std::nullopt});
    for (auto& segment : first.segments) {
        segment.init = 0;
        segment.key = 0;
    }
    ldm::core::FillTrack(track, first);
    ASSERT_EQ(track.pieces.size(), 3u);  // init and two segments

    TrackSegments next = Listed(2, 3);
    next.inits = {{kBase + "init-a.mp4", std::nullopt}, {kBase + "init-b.mp4", std::nullopt}};
    next.keys = {{"", ldm::model::AesKey{9}}, {kBase + "key-a", std::nullopt}};
    next.segments[0].init = 0;
    next.segments[0].key = 1;
    next.segments[1].init = 0;  // segment 3: known init, embedded key
    next.segments[1].key = 0;
    next.segments[2].init = 1;  // segment 4: a new init section
    next.segments[2].key = 0;
    next.segments[2].discontinuity = true;
    const auto append = AppendTrack(track, next);
    EXPECT_EQ(append.firstNewKey, 1u);
    ASSERT_EQ(track.keys.size(), 2u);
    EXPECT_EQ(track.keys[1].value, (ldm::model::AesKey{9}));
    ASSERT_EQ(track.pieces.size(), 6u);  // segment 3, init b, segment 4
    EXPECT_EQ(track.pieces[3].init, 0u);
    EXPECT_EQ(track.pieces[3].key, 1u);
    EXPECT_EQ(track.pieces[4].kind, MediaPiece::Kind::Init);
    EXPECT_EQ(track.pieces[4].url, kBase + "init-b.mp4");
    EXPECT_EQ(track.pieces[5].init, 4u);
    EXPECT_EQ(track.pieces[5].key, 1u);
    EXPECT_TRUE(track.pieces[5].discontinuity);
}

TEST(AppendTrack, ATrackAtTheSegmentLimitStopsRecording)
{
    MediaTrack track = Recording(0, 1);
    track.pieces.resize(ldm::media::kMaxSegmentsPerTrack - 1, track.pieces[0]);
    track.pieces.back().sequence = 1'000'000;
    const auto append = AppendTrack(track, Listed(1'000'001, 3));
    EXPECT_TRUE(append.changed);
    EXPECT_FALSE(track.live) << "as if the stream had ended";
    EXPECT_LE(track.pieces.size(), ldm::media::kMaxSegmentsPerTrack);
}

TEST(MuxRequest, SkippedPiecesAreLeftOutAndTheGapMarked)
{
    ldm::model::Download download;
    download.kind = ldm::model::DownloadKind::Hls;
    MediaTrack track;
    TrackSegments listed = Listed(1, 4);
    listed.inits.push_back({kBase + "init.mp4", std::nullopt});
    for (auto& segment : listed.segments) {
        segment.init = 0;
    }
    ldm::core::FillTrack(track, listed);  // init, then segments 1 to 4 at places 1 to 4
    for (auto& piece : track.pieces) {
        piece.done = true;
    }
    track.pieces[2].done = false;
    ldm::core::SkipUnfetchedPieces(track);
    EXPECT_TRUE(track.pieces[2].skipped);
    download.tracks.push_back(track);

    const auto request = ldm::core::BuildMuxRequest(download, "C:\\parts");
    const auto& pieces = request.inputs.at(0).pieces;
    ASSERT_EQ(pieces.size(), 4u);
    EXPECT_TRUE(pieces[0].init);
    EXPECT_EQ(pieces[1].initPiece, 0u);
    EXPECT_FALSE(pieces[1].discontinuity);
    EXPECT_EQ(pieces[2].file.filename(), L"000003.ts") << "files keep their names";
    EXPECT_TRUE(pieces[2].discontinuity);
    EXPECT_FALSE(pieces[3].discontinuity);

    // A skipped init section takes its segments with it.
    download.tracks[0].pieces[0].skipped = true;
    const auto withoutInit = ldm::core::BuildMuxRequest(download, "C:\\parts");
    EXPECT_TRUE(withoutInit.inputs.at(0).pieces.empty());
}

TEST(MediaRequest, SkippedPiecesAreNotFetchedAndSummariesLeaveThemOut)
{
    ldm::model::Download download;
    download.kind = ldm::model::DownloadKind::Hls;
    MediaTrack track = Recording(1, 4);
    track.pieces[0].done = true;
    track.pieces[0].bytes = 10;
    track.pieces[1].skipped = true;
    download.tracks.push_back(track);
    const auto request = ldm::core::BuildMediaRequest(download, "C:\\parts");
    ASSERT_EQ(request.tracks.at(0).pieces.size(), 2u);
    EXPECT_EQ(request.tracks[0].pieces[0].index, 2u);
    EXPECT_TRUE(request.tracks[0].live);

    const auto summary = ldm::core::SummarizeStream(download);
    EXPECT_EQ(summary.piecesTotal, 3);
    EXPECT_EQ(summary.piecesDone, 1);
    EXPECT_TRUE(summary.live);
    EXPECT_EQ(summary.recordedUs, 2'000'000);
}

}  // namespace
