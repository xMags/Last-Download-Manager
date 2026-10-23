#include "ui/CaptureFollowUp.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

TEST(CaptureFollowUp, SecondCancelForASiteOffersAnException)
{
    ldm::ui::CancelCounter counter;
    EXPECT_FALSE(counter.Cancelled("https://files.test/a.zip"));
    EXPECT_FALSE(counter.Cancelled("https://other.test/a.zip"));
    const auto offer = counter.Cancelled("https://files.test/b.zip");
    ASSERT_TRUE(offer);
    EXPECT_EQ(offer->host, "files.test");
    EXPECT_EQ(offer->url, "https://files.test/b.zip");
    EXPECT_FALSE(offer->sameAddress);

    EXPECT_FALSE(counter.Cancelled("https://files.test/c.zip")) << "counting starts over after an offer";
    const auto again = counter.Cancelled("https://files.test/c.zip");
    ASSERT_TRUE(again);
    EXPECT_TRUE(again->sameAddress);
    EXPECT_FALSE(counter.Cancelled("not an address"));
}

TEST(CaptureFollowUp, SuggestedAddressPatternCoversTheFolder)
{
    EXPECT_EQ(ldm::ui::SuggestAddressPattern("https://a.test/dl/x.zip?t=1#f"), "https://a.test/dl/*");
    EXPECT_EQ(ldm::ui::SuggestAddressPattern("https://a.test/x.zip"), "https://a.test/*");
    EXPECT_EQ(ldm::ui::SuggestAddressPattern("https://a.test"), "https://a.test/*");
    EXPECT_EQ(ldm::ui::SuggestAddressPattern("https://a.test?q=a/b"), "https://a.test/*");
    EXPECT_FALSE(ldm::ui::SuggestAddressPattern("javascript:x"));
}

TEST(CaptureFollowUp, ListsGainAWordOnce)
{
    EXPECT_EQ(ldm::ui::AddToList("", "a.test", ' '), "a.test");
    EXPECT_EQ(ldm::ui::AddToList("x.test y.test", "a.test", ' '), "x.test y.test a.test");
    EXPECT_EQ(ldm::ui::AddToList("x.test A.TEST", "a.test", ' '), "x.test A.TEST");
    EXPECT_EQ(ldm::ui::AddToList("https://x/*", "https://y/*", '\n'), "https://x/*\nhttps://y/*");
    EXPECT_EQ(ldm::ui::AddToList("https://x/*\n", "https://y/*", '\n'), "https://x/*\nhttps://y/*");
}

TEST(CaptureFollowUp, NewAddressMatchesByNameAndSize)
{
    ldm::core::DownloadView waiting;
    waiting.download.fileName = "Setup.zip";
    waiting.download.totalSize = 100;
    EXPECT_TRUE(ldm::ui::IsNewAddressFor(waiting, std::string("setup.zip"), 100));
    EXPECT_TRUE(ldm::ui::IsNewAddressFor(waiting, std::string("setup.zip"), std::nullopt)) << "size unknown";
    EXPECT_FALSE(ldm::ui::IsNewAddressFor(waiting, std::string("setup.zip"), 101));
    EXPECT_FALSE(ldm::ui::IsNewAddressFor(waiting, std::string("other.zip"), 100));
    EXPECT_FALSE(ldm::ui::IsNewAddressFor(waiting, std::nullopt, 100));
    waiting.download.totalSize.reset();
    EXPECT_TRUE(ldm::ui::IsNewAddressFor(waiting, std::string("SETUP.ZIP"), 5));
}

TEST(CaptureFollowUp, PreventKeysAreNamed)
{
    ldm::model::CaptureKeys keys;
    EXPECT_EQ(ldm::ui::PreventKeysText(keys), L"Alt");
    keys.preventShift = true;
    keys.preventCtrl = true;
    EXPECT_EQ(ldm::ui::PreventKeysText(keys), L"Alt+Shift+Ctrl");
    keys.preventEnabled = false;
    EXPECT_EQ(ldm::ui::PreventKeysText(keys), L"");
}
