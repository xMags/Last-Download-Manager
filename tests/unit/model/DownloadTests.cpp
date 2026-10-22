#include "model/Download.h"

#include <gtest/gtest.h>

namespace {

using ldm::model::CompletionAction;
using ldm::model::Download;
using ldm::model::DownloadStatus;
using ldm::model::Segment;

TEST(DownloadStatus, NamesRoundTrip)
{
    for (DownloadStatus status : {DownloadStatus::Queued, DownloadStatus::Downloading, DownloadStatus::Stopping,
                                  DownloadStatus::Paused, DownloadStatus::Finalizing, DownloadStatus::Completed,
                                  DownloadStatus::Failed, DownloadStatus::Deleting}) {
        const auto parsed = ldm::model::ParseDownloadStatus(ldm::model::ToString(status));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, status);
    }
    EXPECT_FALSE(ldm::model::ParseDownloadStatus("Downloading").has_value());
}

TEST(CompletionAction, NamesRoundTrip)
{
    for (CompletionAction action : {CompletionAction::None, CompletionAction::ExitApp, CompletionAction::Shutdown,
                                    CompletionAction::Sleep, CompletionAction::Hibernate}) {
        EXPECT_EQ(ldm::model::ParseCompletionAction(ldm::model::ToString(action)), action);
    }
}

TEST(Segment, CompletionNeedsKnownEnd)
{
    Segment open{0, std::nullopt, 500};
    EXPECT_FALSE(open.IsComplete());
    Segment bounded{100, 200, 100};
    EXPECT_TRUE(bounded.IsComplete());
    EXPECT_EQ(bounded.WriteOffset(), 200);
}

TEST(Download, DownloadedBytesSumsSegments)
{
    Download download;
    download.segments = {{0, 100, 100}, {100, 300, 50}, {300, std::nullopt, 7}};
    EXPECT_EQ(download.DownloadedBytes(), 157);
}

}  // namespace
