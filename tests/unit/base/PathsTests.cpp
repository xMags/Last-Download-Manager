#include "base/Paths.h"

#include <gtest/gtest.h>

namespace {

using ldm::base::ToExtendedLengthPath;

TEST(Paths, PrefixesAbsoluteDrivePaths)
{
    EXPECT_EQ(ToExtendedLengthPath(LR"(C:\Downloads\a.zip)"), LR"(\\?\C:\Downloads\a.zip)");
    EXPECT_EQ(ToExtendedLengthPath(L"C:/Downloads/sub/../a.zip"), LR"(\\?\C:\Downloads\a.zip)");
}

TEST(Paths, ConvertsUncPaths)
{
    EXPECT_EQ(ToExtendedLengthPath(LR"(\\server\share\a.zip)"), LR"(\\?\UNC\server\share\a.zip)");
}

TEST(Paths, KeepsPrefixedAndRelativePaths)
{
    EXPECT_EQ(ToExtendedLengthPath(LR"(\\?\C:\a.zip)"), LR"(\\?\C:\a.zip)");
    EXPECT_EQ(ToExtendedLengthPath(L"relative\\a.zip"), L"relative\\a.zip");
}

}  // namespace
