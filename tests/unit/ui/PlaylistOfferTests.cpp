#include "ui/PlaylistOffer.h"

#include <gtest/gtest.h>

namespace {

TEST(PlaylistOffer, OnlyM3uFilesAreReadAsPlaylists)
{
    EXPECT_TRUE(ldm::ui::IsPlaylistFileName("Top 40.m3u"));
    EXPECT_TRUE(ldm::ui::IsPlaylistFileName("radio.M3U8"));
    EXPECT_FALSE(ldm::ui::IsPlaylistFileName("song.mp3"));
    EXPECT_FALSE(ldm::ui::IsPlaylistFileName("m3u"));
    EXPECT_FALSE(ldm::ui::IsPlaylistFileName("list.m3u.txt"));
}

}  // namespace
