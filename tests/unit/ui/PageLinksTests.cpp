#include "ui/PageLinks.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

namespace {

using ldm::browser::LinkKind;

ldm::browser::Links SampleLinks(bool selection = false)
{
    ldm::browser::Links links;
    links.selection = selection;
    links.userAgent = "UA";
    links.links = {
        {"https://a.test/files/setup.zip", "Setup", std::nullopt, LinkKind::Link},             // 0
        {"https://a.test/about.html", "About", std::nullopt, LinkKind::Link},                  // 1
        {"https://a.test/", "Home", std::nullopt, LinkKind::Link},                             // 2
        {"https://a.test/img/logo.png", "", std::nullopt, LinkKind::Image},                    // 3
        {"https://a.test/get?id=7", "Manual", std::string("manual.pdf"), LinkKind::Link},      // 4
        {"https://a.test/files/setup.zip#mirror", "Mirror", std::nullopt, LinkKind::Link},     // 5
        {"https://a.test/download.php?f=x", "Download", std::nullopt, LinkKind::Link},         // 6
        {"https://ads.test/frame", "", std::nullopt, LinkKind::Frame},                         // 7
    };
    return links;
}

std::vector<size_t> Visible(const std::vector<ldm::ui::LinkRow>& rows, bool html, bool images, bool repeated)
{
    return ldm::ui::VisibleLinkRows(rows, ldm::ui::LinkFilter{html, images, repeated});
}

TEST(PageLinks, RowsKeepTheExtensionsOrderAndNames)
{
    const auto rows = ldm::ui::MakeLinkRows(SampleLinks(), {});
    ASSERT_EQ(rows.size(), 8u);
    EXPECT_EQ(rows[0].fileName, "setup.zip");
    EXPECT_FALSE(rows[0].chosenName);
    EXPECT_EQ(rows[2].fileName, "") << "the address names no file";
    EXPECT_EQ(rows[4].fileName, "manual.pdf") << "the page's download attribute";
    EXPECT_EQ(rows[4].chosenName, "manual.pdf");
    EXPECT_FALSE(rows[0].checked) << "all links start unchecked";
    EXPECT_TRUE(ldm::ui::MakeLinkRows(SampleLinks(true), {})[0].checked) << "selected links start checked";
}

TEST(PageLinks, FiltersHidePagesPicturesAndRepeats)
{
    const auto rows = ldm::ui::MakeLinkRows(SampleLinks(), {"https://a.test/get?id=7"});
    EXPECT_EQ(Visible(rows, false, false, false), (std::vector<size_t>{0, 1, 2, 3, 4, 5, 6, 7}));
    EXPECT_EQ(Visible(rows, true, false, false), (std::vector<size_t>{0, 3, 4, 5}));
    EXPECT_EQ(Visible(rows, false, true, false), (std::vector<size_t>{0, 1, 2, 4, 5, 6, 7}));
    // 5 repeats 0 apart from its fragment; 4 is already in LDM's list.
    EXPECT_EQ(Visible(rows, false, false, true), (std::vector<size_t>{0, 1, 2, 3, 6, 7}));
}

TEST(PageLinks, DetailsDecideWhatIsAPage)
{
    auto rows = ldm::ui::MakeLinkRows(SampleLinks(), {});
    EXPECT_TRUE(ldm::ui::IsHtmlLink(rows[6])) << ".php looks like a page until checked";
    ldm::browser::LinkDetails details{6};
    details.fileSize = 2048;
    details.mimeType = "Application/Zip; name=x";
    details.fileName = "x.zip";
    EXPECT_TRUE(ldm::ui::ApplyLinkDetails(rows, details));
    EXPECT_FALSE(ldm::ui::IsHtmlLink(rows[6]));
    EXPECT_EQ(rows[6].mimeType, "application/zip");
    EXPECT_EQ(rows[6].size, 2048);
    EXPECT_EQ(rows[6].fileName, "x.zip") << "the server's name";
    EXPECT_EQ(rows[6].check, ldm::ui::LinkCheck::Done);
    EXPECT_EQ(ldm::ui::LinkTypeText(rows[6]), "ZIP");

    ldm::browser::LinkDetails page{0};
    page.mimeType = "text/html";
    ldm::ui::ApplyLinkDetails(rows, page);
    EXPECT_TRUE(ldm::ui::IsHtmlLink(rows[0])) << "a .zip address that answers with a page";

    ldm::browser::LinkDetails named{4};
    named.fileName = "other.pdf";
    ldm::ui::ApplyLinkDetails(rows, named);
    EXPECT_EQ(rows[4].fileName, "manual.pdf") << "the page's chosen name stays";

    EXPECT_FALSE(ldm::ui::ApplyLinkDetails(rows, ldm::browser::LinkDetails{8}));
    ldm::browser::LinkDetails failed{1};
    failed.failed = true;
    ldm::ui::ApplyLinkDetails(rows, failed);
    EXPECT_EQ(rows[1].check, ldm::ui::LinkCheck::Failed);
    EXPECT_EQ(ldm::ui::LinkTypeText(rows[2]), "HTML");
}

TEST(PageLinks, PatternNamesNumberTheFiles)
{
    EXPECT_EQ(ldm::ui::NameFromPattern("photo*", 3, 12, "IMG_9.JPG"), "photo03.JPG");
    EXPECT_EQ(ldm::ui::NameFromPattern("photo*.jpg", 1, 5, "a.png"), "photo1.jpg");
    EXPECT_EQ(ldm::ui::NameFromPattern("part *", 7, 9, "readme"), "part 7");
    EXPECT_EQ(ldm::ui::NameFromPattern("v1.2-*", 2, 2, "a.zip"), "v1.2-2.zip");
    EXPECT_EQ(ldm::ui::NameFromPattern("a/b*", 1, 1, "x.txt"), "a_b1.txt") << "made safe as a file name";
    EXPECT_FALSE(ldm::ui::NameFromPattern("photo", 1, 1, "a.jpg"));
    EXPECT_FALSE(ldm::ui::NameFromPattern("*-*", 1, 1, "a.jpg"));
}

TEST(PageLinks, EachAddressGetsOnlyItsOwnCookies)
{
    const std::vector<ldm::model::Cookie> cookies{
        {"a.test", true, "/", false, 0, "site", "1"},
        {"cdn.test", false, "/", true, 0, "cdn", "2"},
        {"a.test", true, "/private", false, 0, "deep", "3"},
    };
    const auto names = [&](const std::string& url) {
        std::vector<std::string> result;
        for (const auto& cookie : ldm::ui::CookiesForAddress(cookies, url)) {
            result.push_back(cookie.name);
        }
        return result;
    };
    EXPECT_EQ(names("https://a.test/f.zip"), std::vector<std::string>{"site"});
    EXPECT_EQ(names("https://a.test/private/f.zip"), (std::vector<std::string>{"site", "deep"}));
    EXPECT_EQ(names("https://img.cdn.test/p.png"), std::vector<std::string>{"cdn"});
    EXPECT_EQ(names("http://img.cdn.test/p.png"), std::vector<std::string>{});
    EXPECT_EQ(names("not an address"), std::vector<std::string>{});
}

}  // namespace
