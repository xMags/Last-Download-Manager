#include "ui/GrabberFields.h"
#include "ui/GrabberView.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace {

using ldm::model::GrabberItem;
using ldm::model::GrabberItemKind;
using ldm::model::GrabberItemState;
using ldm::ui::GrabberNode;

GrabberItem File(std::string url, GrabberItemState state = GrabberItemState::Found, bool checked = true)
{
    GrabberItem item;
    item.url = std::move(url);
    item.kind = GrabberItemKind::File;
    item.state = state;
    item.checked = checked;
    return item;
}

TEST(GrabberFields, ParsesMasks)
{
    EXPECT_EQ(ldm::ui::ParseMasks(L"  *.jpg\t*.png  "), (std::vector<std::string>{"*.jpg", "*.png"}));
    EXPECT_EQ(ldm::ui::ParseMasks(L""), std::vector<std::string>{});
    EXPECT_EQ(ldm::ui::JoinMasks({"*.jpg", "/docs/"}), L"*.jpg /docs/");
    EXPECT_FALSE(ldm::ui::ParseMasks(std::wstring(300, L'a')));
    std::wstring many;
    for (int i = 0; i < 65; ++i) {
        many += L"*.x ";
    }
    EXPECT_FALSE(ldm::ui::ParseMasks(many));
}

TEST(GrabberFields, ReadsTheStartPage)
{
    EXPECT_EQ(ldm::ui::ParseStartPage(L"  example.com/gallery "), "https://example.com/gallery");
    EXPECT_EQ(ldm::ui::ParseStartPage(L"http://example.com/"), "http://example.com/");
    EXPECT_FALSE(ldm::ui::ParseStartPage(L""));
    EXPECT_FALSE(ldm::ui::ParseStartPage(L"ftp://example.com/"));
    EXPECT_FALSE(ldm::ui::ParseStartPage(L"https://"));
    EXPECT_EQ(ldm::ui::SuggestedProjectName("https://www.example.com/a"), L"example.com");
    EXPECT_EQ(ldm::ui::SuggestedProjectName("nonsense"), L"New project");
    EXPECT_EQ(ldm::ui::TemplateChoices().size(), 7u);
}

TEST(GrabberView, SortsItemsIntoTheTreesNodes)
{
    const GrabberItem jpg = File("https://example.com/a.JPG");
    const GrabberItem zip = File("https://cdn.example.com/b.zip", GrabberItemState::Done);
    const GrabberItem failed = File("https://example.com/c.zip", GrabberItemState::Failed, false);
    const GrabberItem leftOut = File("https://example.com/d.gif", GrabberItemState::Filtered, false);
    GrabberItem page;
    page.url = "https://example.com/";
    page.kind = GrabberItemKind::Page;
    const std::string host = "example.com";

    const auto in = [&host](const GrabberItem& item, GrabberNode::Kind kind, std::wstring type = {}) {
        return ldm::ui::InGrabberNode(item, GrabberNode{kind, std::move(type)}, host);
    };
    EXPECT_TRUE(in(jpg, GrabberNode::Kind::AllFiles));
    EXPECT_FALSE(in(leftOut, GrabberNode::Kind::AllFiles));
    EXPECT_FALSE(in(page, GrabberNode::Kind::AllFiles));
    EXPECT_TRUE(in(jpg, GrabberNode::Kind::Checked));
    EXPECT_FALSE(in(failed, GrabberNode::Kind::Checked));
    EXPECT_TRUE(in(zip, GrabberNode::Kind::Downloaded));
    EXPECT_TRUE(in(failed, GrabberNode::Kind::Failed));
    EXPECT_TRUE(in(jpg, GrabberNode::Kind::Type, L"JPG"));
    EXPECT_FALSE(in(zip, GrabberNode::Kind::Type, L"JPG"));
    EXPECT_TRUE(in(zip, GrabberNode::Kind::OtherSites));
    EXPECT_FALSE(in(jpg, GrabberNode::Kind::OtherSites));
    EXPECT_TRUE(in(leftOut, GrabberNode::Kind::LeftOut));
    EXPECT_TRUE(in(page, GrabberNode::Kind::Pages));

    std::vector<std::shared_ptr<const GrabberItem>> items{std::make_shared<GrabberItem>(jpg), std::make_shared<GrabberItem>(zip),
                                                          std::make_shared<GrabberItem>(failed), std::make_shared<GrabberItem>(leftOut),
                                                          std::make_shared<GrabberItem>(page),
                                                          std::make_shared<GrabberItem>(File("https://example.com/readme"))};
    EXPECT_EQ(ldm::ui::GrabberTypes(items), (std::vector<std::wstring>{L"", L"JPG", L"ZIP"}));
}

TEST(GrabberView, DescribesItems)
{
    GrabberItem page;
    page.url = "https://example.com/view.php?id=3";
    page.kind = GrabberItemKind::Page;
    page.state = GrabberItemState::Done;
    EXPECT_EQ(ldm::ui::GrabberItemName(page), L"view.php?id=3");
    EXPECT_EQ(ldm::ui::GrabberItemType(page), L"Page");
    EXPECT_EQ(ldm::ui::GrabberItemStatus(page, nullptr), L"Explored");
    EXPECT_EQ(ldm::ui::GrabberItemName(File("https://example.com/a%20b.zip")), L"a b.zip");
    EXPECT_EQ(ldm::ui::GrabberItemStatus(File("https://example.com/a.zip", GrabberItemState::Filtered), nullptr),
              L"Left out by the filters");
    EXPECT_EQ(ldm::ui::GrabberItemStatus(File("https://example.com/a.zip", GrabberItemState::Downloading), nullptr),
              L"Downloading");

    ldm::grabber::GrabberProjectView view;
    view.project.name = "Site";
    view.exploring = true;
    view.stats.pagesExplored = 3;
    view.stats.pagesWaiting = 2;
    view.stats.filesFound = 1;
    view.stats.filesChecked = 1;
    EXPECT_EQ(ldm::ui::GrabberStatusText(view), L"Exploring: 3 pages read, 2 waiting. 1 file listed, 1 checked, 0 downloaded.");
    view.damaged = "bad";
    EXPECT_NE(ldm::ui::GrabberStatusText(view).find(L"damaged"), std::wstring::npos);
    EXPECT_NE(ldm::ui::GrabberStatisticsText(view).find(L"Files listed: 1"), std::wstring::npos);
}

}  // namespace
