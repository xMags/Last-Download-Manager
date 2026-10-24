#include "ui/PanelPlacement.h"

#include <gtest/gtest.h>

namespace {

using ldm::ui::PanelPosition;
using ldm::ui::VideoOnScreen;

constexpr RECT kPage{100, 200, 1900, 1200};  // the page area on screen
constexpr SIZE kMinimum{100, 60};

TEST(PanelPlacement, VideoRectanglesScaleWithThePixelRatio)
{
    const auto video = VideoOnScreen(kPage, {10, 20, 640, 360}, 1.5, kMinimum);
    ASSERT_TRUE(video);
    EXPECT_EQ(video->left, 115);
    EXPECT_EQ(video->top, 230);
    EXPECT_EQ(video->right, 115 + 960);
    EXPECT_EQ(video->bottom, 230 + 540);
}

TEST(PanelPlacement, VideosAreClippedToThePageAndDroppedWhenBarelyVisible)
{
    const auto scrolled = VideoOnScreen(kPage, {0, -300, 800, 450}, 1, kMinimum);
    ASSERT_TRUE(scrolled);
    EXPECT_EQ(scrolled->top, 200) << "the part above the page is not on screen";
    EXPECT_EQ(scrolled->bottom, 350);
    EXPECT_FALSE(VideoOnScreen(kPage, {0, -420, 800, 450}, 1, kMinimum)) << "30 pixels show";
    EXPECT_FALSE(VideoOnScreen(kPage, {0, 2000, 800, 450}, 1, kMinimum)) << "below the page";
}

TEST(PanelPlacement, ThePanelSitsAboveTheTopRightCornerWhenThereIsRoom)
{
    const RECT video{300, 500, 1300, 1060};
    const POINT at = PanelPosition(kPage, video, SIZE{200, 30}, 4);
    EXPECT_EQ(at.x, 1100);
    EXPECT_EQ(at.y, 466);
}

TEST(PanelPlacement, ThePanelGoesInsideWhenTheVideoTouchesThePageTop)
{
    const RECT video{300, 210, 1300, 770};
    const POINT at = PanelPosition(kPage, video, SIZE{200, 30}, 4);
    EXPECT_EQ(at.y, 214);
}

TEST(PanelPlacement, ThePanelStaysOnThePageAndOnTheVideo)
{
    const RECT narrow{150, 500, 250, 600};  // narrower than the panel
    EXPECT_EQ(PanelPosition(kPage, narrow, SIZE{200, 30}, 4).x, 150) << "starts at the video's left edge";
    const RECT pastRight{1700, 500, 2100, 800};
    EXPECT_EQ(PanelPosition(kPage, pastRight, SIZE{200, 30}, 4).x, 1700);
    const RECT atBottom{300, 1180, 1300, 1200};
    EXPECT_LE(PanelPosition(kPage, atBottom, SIZE{200, 30}, 4).y + 30, kPage.bottom);
}

}  // namespace
