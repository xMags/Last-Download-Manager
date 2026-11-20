#include "ui/Format.h"

#include <gtest/gtest.h>

namespace {

using ldm::ui::FriendlyFolder;

const std::filesystem::path kDownloads = L"C:\\Users\\Dev\\Downloads";

TEST(FriendlyFolder, NamesTheDownloadsFolderAndFoldersInsideIt)
{
    EXPECT_EQ(FriendlyFolder(L"C:\\Users\\Dev\\Downloads", kDownloads), L"Downloads");
    EXPECT_EQ(FriendlyFolder(L"C:\\Users\\Dev\\Downloads\\", kDownloads), L"Downloads");
    EXPECT_EQ(FriendlyFolder(L"C:\\Users\\Dev\\Downloads\\Video", kDownloads), L"Downloads \u203A Video");
    EXPECT_EQ(FriendlyFolder(L"C:\\Users\\Dev\\Downloads\\Video\\Clips", kDownloads),
              L"Downloads \u203A Video \u203A Clips");
    EXPECT_EQ(FriendlyFolder(L"c:\\users\\dev\\DOWNLOADS\\Video", kDownloads), L"Downloads \u203A Video")
        << "letter case does not matter on Windows";
    EXPECT_EQ(FriendlyFolder(L"C:\\Users\\Dev\\Downloads\\Video", L"C:\\Users\\Dev\\Downloads\\"),
              L"Downloads \u203A Video");
}

TEST(FriendlyFolder, ShowsOtherFoldersInFull)
{
    EXPECT_EQ(FriendlyFolder(L"D:\\Media\\Video", kDownloads), L"D:\\Media\\Video");
    EXPECT_EQ(FriendlyFolder(L"C:\\Users\\Dev\\DownloadsOld\\Video", kDownloads), L"C:\\Users\\Dev\\DownloadsOld\\Video")
        << "a folder whose name only starts the same is not inside";
    EXPECT_EQ(FriendlyFolder(L"C:\\Users\\Dev", kDownloads), L"C:\\Users\\Dev");
    EXPECT_EQ(FriendlyFolder(L"\\\\server\\share\\Video", kDownloads), L"\\\\server\\share\\Video");
    EXPECT_EQ(FriendlyFolder(L"C:\\Users\\Dev\\Downloads\\Video", L""), L"C:\\Users\\Dev\\Downloads\\Video")
        << "without a known Downloads folder, the full path";
}

}  // namespace
