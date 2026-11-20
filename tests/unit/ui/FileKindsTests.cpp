#include "ui/FileKinds.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using ldm::model::Category;
using ldm::ui::FileKind;
using ldm::ui::KindOfCategory;
using ldm::ui::KindOfFile;

Category Seeded(const char* name, const char* extensions)
{
    Category category;
    category.name = name;
    category.extensions = extensions;
    category.builtin = true;
    return category;
}

TEST(FileKinds, BuiltInCategoriesHaveTheirKindByTheirSeededName)
{
    EXPECT_EQ(KindOfCategory(Seeded("Video", "")), FileKind::Video);
    EXPECT_EQ(KindOfCategory(Seeded("Music", "")), FileKind::Music);
    EXPECT_EQ(KindOfCategory(Seeded("General", "")), FileKind::Other);
    Category mine = Seeded("Video", "");
    mine.builtin = false;
    EXPECT_EQ(KindOfCategory(mine), FileKind::Other) << "a user category with the same name";
}

TEST(FileKinds, AFileTakesTheKindOfTheBuiltInCategoryListingItsExtension)
{
    const std::vector<Category> categories{Seeded("Compressed", "zip rar 7z"), Seeded("Video", "mp4 mkv webm"),
                                           Seeded("Music", "mp3 flac")};
    EXPECT_EQ(KindOfFile("clip.mp4", categories), FileKind::Video);
    EXPECT_EQ(KindOfFile("Clip.MKV", categories), FileKind::Video) << "letter case does not matter";
    EXPECT_EQ(KindOfFile("song.flac", categories), FileKind::Music);
    EXPECT_EQ(KindOfFile("backup.7z", categories), FileKind::Compressed);
    EXPECT_EQ(KindOfFile("notes.txt", categories), FileKind::Other);
    EXPECT_EQ(KindOfFile("README", categories), FileKind::Other);
    EXPECT_EQ(KindOfFile("clip.mp", categories), FileKind::Other) << "a prefix of a listed extension";
    Category mine = Seeded("Mine", "txt");
    mine.builtin = false;
    EXPECT_EQ(KindOfFile("notes.txt", {mine}), FileKind::Other) << "user categories have no kind";
}

}  // namespace
