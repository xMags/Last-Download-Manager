#include "ui/BatchPattern.h"
#include "ui/History.h"
#include "ui/SystemActions.h"

#include <gtest/gtest.h>

namespace {

using ldm::ui::BatchPattern;
using ldm::ui::ExpandBatch;

TEST(BatchPattern, NumbersWithPadding)
{
    BatchPattern pattern;
    pattern.address = "http://x.test/img*.jpg";
    pattern.fromNumber = 1;
    pattern.toNumber = 12;
    pattern.width = 3;
    const auto expansion = ExpandBatch(pattern);
    ASSERT_FALSE(expansion.error);
    ASSERT_EQ(expansion.addresses.size(), 12u);
    EXPECT_EQ(expansion.addresses.front(), "http://x.test/img001.jpg");
    EXPECT_EQ(expansion.addresses[1], "http://x.test/img002.jpg");
    EXPECT_EQ(expansion.addresses.back(), "http://x.test/img012.jpg");
}

TEST(BatchPattern, BackwardsAndLetters)
{
    BatchPattern numbers;
    numbers.address = "http://x.test/p*";
    numbers.fromNumber = 3;
    numbers.toNumber = 1;
    EXPECT_EQ(ExpandBatch(numbers).addresses, (std::vector<std::string>{"http://x.test/p3", "http://x.test/p2", "http://x.test/p1"}));

    BatchPattern letters;
    letters.address = "http://x.test/disk_*.iso";
    letters.letters = true;
    letters.fromLetter = 'a';
    letters.toLetter = 'c';
    EXPECT_EQ(ExpandBatch(letters).addresses,
              (std::vector<std::string>{"http://x.test/disk_a.iso", "http://x.test/disk_b.iso", "http://x.test/disk_c.iso"}));
}

TEST(BatchPattern, RejectsBadInput)
{
    BatchPattern pattern;
    pattern.address = "http://x.test/none.jpg";
    EXPECT_TRUE(ExpandBatch(pattern).error);
    pattern.address = "http://x.test/*/*.jpg";
    EXPECT_TRUE(ExpandBatch(pattern).error);
    pattern.address = "http://x.test/*.jpg";
    pattern.fromNumber = 0;
    pattern.toNumber = 1000;  // 1001 files
    EXPECT_TRUE(ExpandBatch(pattern).error);
    pattern.toNumber = 999;
    EXPECT_EQ(ExpandBatch(pattern).addresses.size(), 1000u);
    pattern.letters = true;
    pattern.fromLetter = 'a';
    pattern.toLetter = 'Z';
    EXPECT_TRUE(ExpandBatch(pattern).error);
}

TEST(History, PushesNewestFirstWithoutDuplicates)
{
    std::string history;
    history = ldm::ui::PushHistory(history, "http://a", 3);
    history = ldm::ui::PushHistory(history, "http://b", 3);
    history = ldm::ui::PushHistory(history, "HTTP://A", 3);
    EXPECT_EQ(ldm::ui::HistoryEntries(history), (std::vector<std::string>{"HTTP://A", "http://b"}));
    history = ldm::ui::PushHistory(history, "http://c", 3);
    history = ldm::ui::PushHistory(history, "http://d", 3);
    EXPECT_EQ(ldm::ui::HistoryEntries(history), (std::vector<std::string>{"http://d", "http://c", "HTTP://A"}));
    EXPECT_EQ(ldm::ui::PushHistory(history, "bad\nentry", 3), history);
}

TEST(SystemActions, VirusScanArguments)
{
    using ldm::ui::SubstituteFileArgument;
    EXPECT_EQ(SubstituteFileArgument(L"/scan [File] /quiet", L"C:\\a b.zip"), L"/scan \"C:\\a b.zip\" /quiet");
    EXPECT_EQ(SubstituteFileArgument(L"/scan \"[File]\"", L"C:\\a b.zip"), L"/scan \"C:\\a b.zip\"");
    EXPECT_EQ(SubstituteFileArgument(L"", L"C:\\a.zip"), L"\"C:\\a.zip\"");
    EXPECT_EQ(SubstituteFileArgument(L"-q", L"C:\\a.zip"), L"-q \"C:\\a.zip\"");
}

}  // namespace
