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
    ldm::ui::BrowserSessions sessions(link, "5.0.0", [&](ldm::browser::Capture capture) { captured.push_back(capture.url); });
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

TEST(AddressExceptions, PatternsAreTrimmedAndMustBeOneWord)
{
    EXPECT_EQ(ldm::ui::CleanAddressPattern(L"  https://a.test/*\r\n"), L"https://a.test/*");
    EXPECT_FALSE(ldm::ui::CleanAddressPattern(L"   "));
    EXPECT_FALSE(ldm::ui::CleanAddressPattern(L"https://a.test/a b"));
    EXPECT_FALSE(ldm::ui::CleanAddressPattern(L"https://a.test/\x7f"));
}

}  // namespace
