#include "ui/BrowserSessions.h"
#include "ui/dialogs/CaptureDialogs.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace {

using ldm::ui::BrowserConnectionId;

class RecordingLink final : public ldm::ui::BrowserLink {
public:
    void Send(BrowserConnectionId connection, std::string json) override { sent.emplace_back(connection, std::move(json)); }
    std::vector<std::pair<BrowserConnectionId, std::string>> sent;
};

ldm::ui::BrowserEvent Event(BrowserConnectionId connection, ldm::browser::Message message, std::string id = "m1")
{
    return {connection, ldm::browser::ParsedMessage{std::move(id), std::move(message)}};
}

ldm::browser::Hello SampleHello()
{
    return {"Chrome", "141", "5.0.0", false, false};
}

TEST(BrowserSessions, HelloIsAnsweredWithTheCaptureRules)
{
    RecordingLink link;
    ldm::ui::BrowserSessions sessions(link, "5.0.0", {});
    ldm::model::Settings settings;
    settings.capturedExtensions = "ZIP .Exe R0*";
    settings.captureSiteExceptions = "*.a.test  b.test/x/*";
    settings.captureAddressExceptions = "https://c.test/*\nhttps://d.test/get?id=*";

    sessions.OnEvent(Event(1, ldm::browser::HostInfo{}), settings);
    sessions.OnEvent(Event(1, SampleHello(), "h1"), settings);
    ASSERT_EQ(link.sent.size(), 1u);
    const auto config = nlohmann::json::parse(link.sent[0].second);
    EXPECT_EQ(config["id"], "h1");
    EXPECT_EQ(config["capturedExtensions"], nlohmann::json({"zip", ".exe", "r0*"}));
    EXPECT_EQ(config["siteExceptions"], nlohmann::json({"*.a.test", "b.test/x/*"}));
    EXPECT_EQ(config["addressExceptions"], nlohmann::json({"https://c.test/*", "https://d.test/get?id=*"}));
    EXPECT_EQ(config["keys"]["prevent"]["alt"], true);
    EXPECT_EQ(config["keys"]["force"]["insert"], true);
    EXPECT_EQ(config["keys"]["forceNeedsLinkClick"], true);
    EXPECT_EQ(config["keys"]["skipWebContent"], true);
}

TEST(BrowserSessions, ChangedKeysArePushed)
{
    RecordingLink link;
    ldm::ui::BrowserSessions sessions(link, "5.0.0", {});
    ldm::model::Settings settings;
    sessions.OnSettingsChanged(settings);
    sessions.OnEvent(Event(1, ldm::browser::HostInfo{}), settings);
    sessions.OnEvent(Event(1, SampleHello()), settings);
    link.sent.clear();
    settings.captureKeys.forceNeedsLinkClick = false;
    sessions.OnSettingsChanged(settings);
    ASSERT_EQ(link.sent.size(), 1u);
    EXPECT_EQ(nlohmann::json::parse(link.sent[0].second)["keys"]["forceNeedsLinkClick"], false);
}

TEST(BrowserSessions, ChangedRulesArePushedToConnectedBrowsersOnly)
{
    RecordingLink link;
    ldm::ui::BrowserSessions sessions(link, "5.0.0", {});
    ldm::model::Settings settings;
    sessions.OnSettingsChanged(settings);  // the starting point
    sessions.OnEvent(Event(1, ldm::browser::HostInfo{}), settings);
    sessions.OnEvent(Event(1, SampleHello()), settings);
    sessions.OnEvent(Event(2, ldm::browser::HostInfo{}), settings);  // no hello yet
    link.sent.clear();

    sessions.OnSettingsChanged(settings);
    EXPECT_TRUE(link.sent.empty()) << "nothing changed";

    settings.captureSiteExceptions = "new.test";
    sessions.OnSettingsChanged(settings);
    ASSERT_EQ(link.sent.size(), 1u);
    EXPECT_EQ(link.sent[0].first, 1u);
    EXPECT_EQ(nlohmann::json::parse(link.sent[0].second)["siteExceptions"], nlohmann::json({"new.test"}));
}

TEST(BrowserSessions, CapturesAreAcceptedOnlyAfterHelloAndWithAValidAddress)
{
    RecordingLink link;
    std::vector<std::string> captured;
    ldm::ui::BrowserSessions::Handlers handlers;
    handlers.onCapture = [&](ldm::browser::Capture capture) { captured.push_back(capture.url); };
    ldm::ui::BrowserSessions sessions(link, "5.0.0", std::move(handlers));
    const ldm::model::Settings settings;
    ldm::browser::Capture capture;
    capture.url = "https://a.test/x.zip";
    capture.userAgent = "UA";

    sessions.OnEvent(Event(1, ldm::browser::HostInfo{}), settings);
    sessions.OnEvent(Event(1, capture, "c1"), settings);
    ASSERT_EQ(link.sent.size(), 1u);
    EXPECT_EQ(nlohmann::json::parse(link.sent[0].second)["accepted"], false) << "no hello yet";

    sessions.OnEvent(Event(1, SampleHello()), settings);
    sessions.OnEvent(Event(1, capture, "c2"), settings);
    EXPECT_EQ(nlohmann::json::parse(link.sent.back().second)["accepted"], true);
    EXPECT_EQ(captured, std::vector<std::string>{"https://a.test/x.zip"});
}

TEST(BrowserSessions, ConfigCarriesTheMenuItemsOfTheBrowsersFamily)
{
    RecordingLink link;
    ldm::ui::BrowserSessions sessions(link, "5.0.0", {});
    ldm::model::Settings settings;
    settings.chromiumMenu.downloadAll = false;
    settings.firefoxMenu.downloadLink = false;
    sessions.OnSettingsChanged(settings);
    sessions.OnEvent(Event(1, ldm::browser::HostInfo{ldm::browser::BrowserFamily::Chromium, "x"}), settings);
    sessions.OnEvent(Event(1, SampleHello()), settings);
    sessions.OnEvent(Event(2, ldm::browser::HostInfo{ldm::browser::BrowserFamily::Firefox, "y"}), settings);
    sessions.OnEvent(Event(2, SampleHello()), settings);
    ASSERT_EQ(link.sent.size(), 2u);
    EXPECT_EQ(nlohmann::json::parse(link.sent[0].second)["menu"], nlohmann::json({{"downloadLink", true}, {"downloadAll", false}}));
    EXPECT_EQ(nlohmann::json::parse(link.sent[1].second)["menu"], nlohmann::json({{"downloadLink", false}, {"downloadAll", true}}));

    link.sent.clear();
    settings.chromiumMenu.downloadAll = true;
    sessions.OnSettingsChanged(settings);
    ASSERT_EQ(link.sent.size(), 2u) << "a menu change is pushed to every browser";
    EXPECT_EQ(nlohmann::json::parse(link.sent[0].second)["menu"]["downloadAll"], true);
}

TEST(BrowserSessions, LinksOpenTheDialogAndTheirDetailsFollow)
{
    RecordingLink link;
    std::vector<ldm::ui::BrowserSessions::LinksRequest> requests;
    std::vector<size_t> detailCounts;
    ldm::ui::BrowserSessions::Handlers handlers;
    handlers.onLinks = [&](ldm::ui::BrowserSessions::LinksRequest request) { requests.push_back(std::move(request)); };
    handlers.onLinkInfo = [&](BrowserConnectionId, ldm::browser::LinkInfo info) { detailCounts.push_back(info.items.size()); };
    ldm::ui::BrowserSessions sessions(link, "5.0.0", std::move(handlers));
    const ldm::model::Settings settings;
    ldm::browser::Links links;
    links.userAgent = "UA";
    links.links.push_back({"https://a.test/x.zip", "X", std::nullopt, ldm::browser::LinkKind::Link});

    sessions.OnEvent(Event(1, ldm::browser::HostInfo{}), settings);
    sessions.OnEvent(Event(1, links, "l1"), settings);
    EXPECT_EQ(nlohmann::json::parse(link.sent.back().second)["accepted"], false) << "no hello yet";
    EXPECT_TRUE(requests.empty());

    sessions.OnEvent(Event(1, SampleHello()), settings);
    sessions.OnEvent(Event(1, links, "l2"), settings);
    const auto answer = nlohmann::json::parse(link.sent.back().second);
    EXPECT_EQ(answer["type"], "links-result");
    EXPECT_EQ(answer["id"], "l2");
    EXPECT_EQ(answer["accepted"], true);
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].connection, 1u);
    EXPECT_EQ(requests[0].requestId, "l2");

    sessions.OnEvent(Event(1, ldm::browser::LinkInfo{{ldm::browser::LinkDetails{0}}}, "i1"), settings);
    EXPECT_EQ(detailCounts, std::vector<size_t>{1});

    sessions.LinksDone(1, "l2");
    const auto done = nlohmann::json::parse(link.sent.back().second);
    EXPECT_EQ(done["type"], "links-done");
    EXPECT_EQ(done["id"], "l2");
    const size_t sentBefore = link.sent.size();
    sessions.OnEvent({1, std::nullopt}, settings);  // the connection closed
    sessions.LinksDone(1, "l2");
    EXPECT_EQ(link.sent.size(), sentBefore) << "nothing goes to a closed connection";
}

TEST(AddressExceptions, PatternsAreTrimmedAndMustBeOneWord)
{
    EXPECT_EQ(ldm::ui::CleanAddressPattern(L"  https://a.test/*\r\n"), L"https://a.test/*");
    EXPECT_FALSE(ldm::ui::CleanAddressPattern(L"   "));
    EXPECT_FALSE(ldm::ui::CleanAddressPattern(L"https://a.test/a b"));
    EXPECT_FALSE(ldm::ui::CleanAddressPattern(L"https://a.test/\x7f"));
}

}  // namespace

TEST(BrowserSessions, OpenPageGoesToTheNewestLastingConnection)
{
    RecordingLink link;
    ldm::ui::BrowserSessions sessions(link, "5.0.0", {});
    const ldm::model::Settings settings;
    EXPECT_FALSE(sessions.OpenPage("https://a.test/page")) << "no browser connected";

    ldm::browser::Hello lasting = SampleHello();
    lasting.startLdm = false;
    ldm::browser::Hello single = SampleHello();
    single.startLdm = true;
    sessions.OnEvent(Event(1, ldm::browser::HostInfo{}), settings);
    sessions.OnEvent(Event(1, lasting), settings);
    sessions.OnEvent(Event(2, ldm::browser::HostInfo{}), settings);
    sessions.OnEvent(Event(2, single), settings);
    link.sent.clear();

    EXPECT_FALSE(sessions.OpenPage("ftp://a.test/page")) << "only web pages";
    EXPECT_FALSE(sessions.OpenPage("javascript:alert(1)"));
    ASSERT_TRUE(sessions.OpenPage("https://a.test/page"));
    ASSERT_EQ(link.sent.size(), 1u);
    EXPECT_EQ(link.sent[0].first, 1u) << "a single request's connection closes soon";
    const auto message = nlohmann::json::parse(link.sent[0].second);
    EXPECT_EQ(message["type"], "open-page");
    EXPECT_EQ(message["url"], "https://a.test/page");
}
