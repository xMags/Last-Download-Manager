#include "ui/CommandState.h"
#include "ui/Format.h"
#include "ui/StopChoice.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using ldm::core::DownloadView;
using ldm::model::DownloadStatus;
using ldm::ui::RecordingStopAnswer;

DownloadView Download(std::int64_t id, DownloadStatus status, bool live)
{
    DownloadView view;
    view.download.id = ldm::model::DownloadId{id};
    view.download.fileName = "Show " + std::to_string(id) + ".mp4";
    view.download.status = status;
    if (live) {
        DownloadView::Stream stream;
        stream.live = true;
        stream.recordedUs = 754'000'000;
        view.stream = stream;
    }
    return view;
}

// Answers `answer` and keeps the questions asked.
struct Asker {
    RecordingStopAnswer answer = RecordingStopAnswer::Finish;
    std::vector<std::wstring> questions;
    RecordingStopAnswer operator()(const std::wstring& question)
    {
        questions.push_back(question);
        return answer;
    }
};

std::vector<ldm::model::DownloadId> Ids(std::initializer_list<std::int64_t> values)
{
    std::vector<ldm::model::DownloadId> ids;
    for (const std::int64_t value : values) {
        ids.push_back(ldm::model::DownloadId{value});
    }
    return ids;
}

TEST(StopChoice, DownloadsThatAreNotRecordingsStopWithoutAQuestion)
{
    const DownloadView file = Download(1, DownloadStatus::Downloading, false);
    const DownloadView* selected[] = {&file};
    Asker asker;
    const auto commands = ldm::ui::StopCommands(selected, std::ref(asker));
    EXPECT_TRUE(asker.questions.empty());
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(std::get<ldm::core::StopCommand>(commands[0]).ids, Ids({1}));
}

TEST(StopChoice, YesFinishesTheRecordingsAndStopsTheRest)
{
    const DownloadView live = Download(1, DownloadStatus::Downloading, true);
    const DownloadView file = Download(2, DownloadStatus::Downloading, false);
    const DownloadView* selected[] = {&live, &file};
    Asker asker;
    const auto commands = ldm::ui::StopCommands(selected, std::ref(asker));
    ASSERT_EQ(asker.questions.size(), 1u);
    EXPECT_EQ(asker.questions[0].rfind(L"Show 1.mp4 is a live stream.", 0), 0u) << asker.questions[0];
    ASSERT_EQ(commands.size(), 2u);
    EXPECT_EQ(std::get<ldm::core::FinishRecordingCommand>(commands[0]).ids, Ids({1}));
    EXPECT_EQ(std::get<ldm::core::StopCommand>(commands[1]).ids, Ids({2}));
}

TEST(StopChoice, NoStopsEverythingAndCancelDoesNothing)
{
    const DownloadView first = Download(1, DownloadStatus::Downloading, true);
    const DownloadView second = Download(2, DownloadStatus::Paused, true);
    const DownloadView* selected[] = {&first, &second};
    Asker no{RecordingStopAnswer::Stop};
    const auto stopped = ldm::ui::StopCommands(selected, std::ref(no));
    ASSERT_EQ(no.questions.size(), 1u) << "one question for every recording";
    EXPECT_NE(no.questions[0].find(L"2 of the selected downloads"), std::wstring::npos);
    ASSERT_EQ(stopped.size(), 1u);
    EXPECT_EQ(std::get<ldm::core::StopCommand>(stopped[0]).ids, Ids({1, 2}));

    Asker cancel{RecordingStopAnswer::Cancel};
    EXPECT_TRUE(ldm::ui::StopCommands(selected, std::ref(cancel)).empty());
}

TEST(StopChoice, APausedRecordingCanBeStoppedToFinishIt)
{
    const DownloadView paused = Download(1, DownloadStatus::Paused, true);
    const DownloadView done = Download(2, DownloadStatus::Completed, true);
    EXPECT_TRUE(ldm::ui::IsLiveRecording(paused));
    EXPECT_FALSE(ldm::ui::IsLiveRecording(done));
    const DownloadView* selected[] = {&paused};
    ldm::core::Snapshot snapshot;
    EXPECT_TRUE(ldm::ui::ComputeCommandState(selected, snapshot).stop);
}

TEST(StopChoice, ARecordingShowsItsLengthInsteadOfAPercentage)
{
    DownloadView live = Download(1, DownloadStatus::Downloading, true);
    live.stream->piecesTotal = 10;
    live.stream->fraction = 0.5;
    EXPECT_EQ(ldm::ui::FormatStatus(live), L"Recording live, 12:34");
    EXPECT_FALSE(ldm::ui::ProgressFraction(live)) << "no bar: a recording has no end";
    live.download.status = DownloadStatus::Paused;
    EXPECT_EQ(ldm::ui::FormatStatus(live), L"Recording paused, 12:34");
    EXPECT_EQ(ldm::ui::FormatRecordedLength(3'723'000'000), L"1:02:03");
}

}  // namespace
