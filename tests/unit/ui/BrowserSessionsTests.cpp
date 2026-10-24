#include "ui/BrowserSessions.h"
#include "ui/dialogs/CaptureDialogs.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <optional>
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

TEST(BrowserSessions, AsksTheBrowserForCookiesAndTakesOnlyItsAnswer)
{
    RecordingLink link;
    ldm::ui::BrowserSessions sessions(link, "5.0.0", {});
    const ldm::model::Settings settings;
    const auto now = std::chrono::steady_clock::now();
    EXPECT_FALSE(sessions.RequestCookies("https://a.test/", [](auto) { ADD_FAILURE(); }, now)) << "no browser yet";

    sessions.OnEvent(Event(1, ldm::browser::HostInfo{}), settings);
    sessions.OnEvent(Event(1, SampleHello()), settings);
    sessions.OnEvent(Event(2, ldm::browser::HostInfo{}), settings);
    sessions.OnEvent(Event(2, SampleHello()), settings);
    link.sent.clear();
    EXPECT_FALSE(sessions.RequestCookies("ftp://a.test/", [](auto) { ADD_FAILURE(); }, now));

    std::vector<std::optional<std::vector<ldm::model::Cookie>>> answers;
    ASSERT_TRUE(sessions.RequestCookies("https://a.test/", [&](auto cookies) { answers.push_back(cookies); },
                                        now + std::chrono::seconds(10)));
    ASSERT_EQ(link.sent.size(), 1u);
    EXPECT_EQ(link.sent[0].first, 2u) << "the newest browser";
    const auto request = nlohmann::json::parse(link.sent[0].second);
    const std::string requestId = request["id"];

    ldm::model::Cookie cookie{"a.test", true, "/", false, 0, "sid", "x"};
    sessions.OnEvent(Event(1, ldm::browser::Cookies{requestId, {cookie}}), settings);
    EXPECT_TRUE(answers.empty()) << "another browser cannot answer";
    sessions.OnEvent(Event(2, ldm::browser::Cookies{requestId, {cookie}}), settings);
    ASSERT_EQ(answers.size(), 1u);
    ASSERT_TRUE(answers[0]);
    EXPECT_EQ(answers[0]->size(), 1u);
    sessions.OnEvent(Event(2, ldm::browser::Cookies{requestId, {cookie}}), settings);
    EXPECT_EQ(answers.size(), 1u) << "answered once";

    // A request whose browser leaves, and one that expires, are answered with nothing.
    ASSERT_TRUE(sessions.RequestCookies("https://a.test/", [&](auto cookies) { answers.push_back(cookies); },
                                        now + std::chrono::seconds(10)));
    sessions.OnEvent(ldm::ui::BrowserEvent{2, std::nullopt}, settings);
    ASSERT_EQ(answers.size(), 2u);
    EXPECT_FALSE(answers[1]);
    ASSERT_TRUE(sessions.RequestCookies("https://a.test/", [&](auto cookies) { answers.push_back(cookies); },
                                        now + std::chrono::seconds(10)));
    sessions.ExpireCookieRequests(now + std::chrono::seconds(5));
    EXPECT_EQ(answers.size(), 2u);
    sessions.ExpireCookieRequests(now + std::chrono::seconds(10));
    ASSERT_EQ(answers.size(), 3u);
    EXPECT_FALSE(answers[2]);
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

TEST(BrowserSessions, MediaReportsArriveOnlyAfterHello)
{
    RecordingLink link;
    std::vector<std::pair<BrowserConnectionId, ldm::browser::MediaFound>> found;
    std::vector<int> resets;
    std::vector<ldm::browser::VideoPosition> positions;
    ldm::ui::BrowserSessions::Handlers handlers;
    handlers.onMedia = [&found](BrowserConnectionId connection, ldm::browser::MediaFound media) {
        found.emplace_back(connection, std::move(media));
    };
    handlers.onMediaReset = [&resets](BrowserConnectionId, int tabId) { resets.push_back(tabId); };
    handlers.onVideoPosition = [&positions](BrowserConnectionId, ldm::browser::VideoPosition position) {
        positions.push_back(position);
    };
    ldm::ui::BrowserSessions sessions(link, "5.0.0", std::move(handlers));
    const ldm::model::Settings settings;
    ldm::browser::MediaFound media;
    media.tabId = 4;
    media.url = "https://cdn.test/m.m3u8";

    sessions.OnEvent(Event(1, ldm::browser::HostInfo{}), settings);
    sessions.OnEvent(Event(1, media), settings);
    EXPECT_TRUE(found.empty()) << "before hello";

    ldm::ui::BrowserEvent hostEvent = Event(2, ldm::browser::HostInfo{});
    hostEvent.hostProcessId = 4242;
    sessions.OnEvent(std::move(hostEvent), settings);
    sessions.OnEvent(Event(2, SampleHello()), settings);
    EXPECT_EQ(sessions.Sessions().at(2).hostProcessId, 4242u);
    sessions.OnEvent(Event(2, media), settings);
    sessions.OnEvent(Event(2, ldm::browser::MediaReset{4}), settings);
    ldm::browser::VideoPosition position;
    position.tabId = 4;
    sessions.OnEvent(Event(2, position), settings);
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].first, 2u);
    EXPECT_EQ(resets, std::vector<int>{4});
    EXPECT_EQ(positions.size(), 1u);

    link.sent.clear();
    sessions.SendMediaList(2, 4, true);
    sessions.SendMediaList(9, 4, true);  // no such connection
    ASSERT_EQ(link.sent.size(), 1u);
    const auto list = nlohmann::json::parse(link.sent[0].second);
    EXPECT_EQ(list["type"], "media-list");
    EXPECT_EQ(list["hasMedia"], true);
}
