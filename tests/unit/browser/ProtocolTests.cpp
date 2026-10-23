#include "browser/HostManifest.h"
#include "browser/Protocol.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>

namespace {

using ldm::browser::BrowserFamily;
using ldm::browser::HostInfo;

constexpr char kChromeId[] = "jdkneeflbkgkkinfcpfamlckpoknbnln";

std::string HelloJson(const std::string& extra = {})
{
    return R"({"v":1,"id":"7","type":"hello","browser":"Chrome","browserVersion":"141.0","extensionVersion":"1.0.0","incognitoAllowed":false)" +
           extra + "}";
}

TEST(BrowserProtocol, EnvelopeNeedsVersionIdAndType)
{
    EXPECT_TRUE(ldm::browser::IsValidEnvelope(R"({"v":1,"id":"a","type":"hello"})"));
    EXPECT_TRUE(ldm::browser::IsValidEnvelope(R"({"v":1,"id":"a","type":"media-download"})"));
    EXPECT_FALSE(ldm::browser::IsValidEnvelope(R"({"v":2,"id":"a","type":"hello"})"));
    EXPECT_FALSE(ldm::browser::IsValidEnvelope(R"({"v":"1","id":"a","type":"hello"})"));
    EXPECT_FALSE(ldm::browser::IsValidEnvelope(R"({"v":1,"type":"hello"})"));
    EXPECT_FALSE(ldm::browser::IsValidEnvelope(R"({"v":1,"id":"","type":"hello"})"));
    EXPECT_FALSE(ldm::browser::IsValidEnvelope(R"({"v":1,"id":"a","type":"Hello"})"));
    EXPECT_FALSE(ldm::browser::IsValidEnvelope(R"({"v":1,"id":"a","type":"he llo"})"));
    EXPECT_FALSE(ldm::browser::IsValidEnvelope(R"({"v":1,"id":"a"})"));
    EXPECT_FALSE(ldm::browser::IsValidEnvelope(R"([1,2])"));
    EXPECT_FALSE(ldm::browser::IsValidEnvelope("not json"));
    EXPECT_FALSE(ldm::browser::IsValidEnvelope(""));
    EXPECT_FALSE(ldm::browser::IsValidEnvelope(R"({"v":1,"id":")" + std::string(65, 'x') + R"(","type":"hello"})"));
}

TEST(BrowserProtocol, EnvelopeRejectsOversizedMessages)
{
    const std::string padding(ldm::browser::kMaxInboundBytes, ' ');
    EXPECT_FALSE(ldm::browser::IsValidEnvelope(R"({"v":1,"id":"a","type":"hello"})" + padding));
}

TEST(BrowserProtocol, ParsesHello)
{
    const auto parsed = ldm::browser::ParseMessage(HelloJson());
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->id, "7");
    const auto* hello = std::get_if<ldm::browser::Hello>(&parsed->message);
    ASSERT_NE(hello, nullptr);
    EXPECT_EQ(hello->browser, "Chrome");
    EXPECT_EQ(hello->browserVersion, "141.0");
    EXPECT_EQ(hello->extensionVersion, "1.0.0");
    EXPECT_FALSE(hello->incognitoAllowed);
}

TEST(BrowserProtocol, RejectsInvalidHello)
{
    EXPECT_FALSE(ldm::browser::ParseMessage(R"({"v":1,"id":"7","type":"hello"})"));
    EXPECT_FALSE(ldm::browser::ParseMessage(
        R"({"v":1,"id":"7","type":"hello","browser":"","browserVersion":"1","extensionVersion":"1","incognitoAllowed":true})"));
    EXPECT_FALSE(ldm::browser::ParseMessage(
        R"({"v":1,"id":"7","type":"hello","browser":"Chrome","browserVersion":"1","extensionVersion":"1","incognitoAllowed":"yes"})"));
    EXPECT_FALSE(ldm::browser::ParseMessage(R"({"v":1,"id":"7","type":"hello","browser":")" + std::string(65, 'x') +
                                            R"(","browserVersion":"1","extensionVersion":"1","incognitoAllowed":true})"));
    // Control characters in display text could forge lines in LDM's UI.
    EXPECT_FALSE(ldm::browser::ParseMessage(
        R"({"v":1,"id":"7","type":"hello","browser":"Chrome\n","browserVersion":"1","extensionVersion":"1","incognitoAllowed":true})"));
}

TEST(BrowserProtocol, RejectsUnknownTypes)
{
    EXPECT_FALSE(ldm::browser::ParseMessage(R"({"v":1,"id":"7","type":"launch-missiles"})"));
}

TEST(BrowserProtocol, HostInfoRoundTrips)
{
    const HostInfo chrome{BrowserFamily::Chromium, kChromeId};
    const auto parsedChrome = ldm::browser::ParseMessage(ldm::browser::EncodeHostInfo(chrome));
    ASSERT_TRUE(parsedChrome);
    const auto* info = std::get_if<HostInfo>(&parsedChrome->message);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->family, BrowserFamily::Chromium);
    EXPECT_EQ(info->extensionId, kChromeId);

    const HostInfo firefox{BrowserFamily::Firefox, "ldm-integration@mrdevfx.github.io"};
    const auto parsedFirefox = ldm::browser::ParseMessage(ldm::browser::EncodeHostInfo(firefox));
    ASSERT_TRUE(parsedFirefox);
    EXPECT_EQ(std::get<HostInfo>(parsedFirefox->message).family, BrowserFamily::Firefox);
}

TEST(BrowserProtocol, HostInfoRejectsMalformedIds)
{
    EXPECT_FALSE(ldm::browser::ParseMessage(R"({"v":1,"id":"h","type":"host","family":"chromium","extensionId":"abc"})"));
    EXPECT_FALSE(ldm::browser::ParseMessage(
        R"({"v":1,"id":"h","type":"host","family":"chromium","extensionId":"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"})"));
    EXPECT_FALSE(ldm::browser::ParseMessage(R"({"v":1,"id":"h","type":"host","family":"firefox","extensionId":"a b"})"));
    EXPECT_FALSE(ldm::browser::ParseMessage(R"({"v":1,"id":"h","type":"host","family":"opera","extensionId":"x@y"})"));
}

TEST(BrowserProtocol, HostInfoFromChromiumArguments)
{
    const auto info = ldm::browser::HostInfoFromArguments(
        {L"chrome-extension://jdkneeflbkgkkinfcpfamlckpoknbnln/", L"--parent-window=0"});
    ASSERT_TRUE(info);
    EXPECT_EQ(info->family, BrowserFamily::Chromium);
    EXPECT_EQ(info->extensionId, kChromeId);

    EXPECT_FALSE(ldm::browser::HostInfoFromArguments({L"chrome-extension://not-an-id/"}));
}

TEST(BrowserProtocol, HostInfoFromFirefoxArguments)
{
    const auto info = ldm::browser::HostInfoFromArguments(
        {L"C:\\Users\\x\\AppData\\Local\\Last Download Manager\\browser\\firefox.json", L"ldm-integration@mrdevfx.github.io"});
    ASSERT_TRUE(info);
    EXPECT_EQ(info->family, BrowserFamily::Firefox);
    EXPECT_EQ(info->extensionId, "ldm-integration@mrdevfx.github.io");

    EXPECT_FALSE(ldm::browser::HostInfoFromArguments({}));
    EXPECT_FALSE(ldm::browser::HostInfoFromArguments({L"manifest.json", L"bad id with spaces"}));
}

TEST(BrowserProtocol, EncodesConfig)
{
    const ldm::browser::Config config{"5.0.0", {"zip", "exe"}};
    const auto json = nlohmann::json::parse(ldm::browser::EncodeConfig(config, "7"));
    EXPECT_EQ(json["v"], 1);
    EXPECT_EQ(json["id"], "7");
    EXPECT_EQ(json["type"], "config");
    EXPECT_EQ(json["appVersion"], "5.0.0");
    EXPECT_EQ(json["capturedExtensions"], nlohmann::json({"zip", "exe"}));
}

TEST(BrowserHostManifest, ChromiumManifestAllowsOnlyLdmExtension)
{
    const auto json = nlohmann::json::parse(
        ldm::browser::NativeHostManifest(BrowserFamily::Chromium, L"C:\\Program Files\\LDM\\ldm-host.exe"));
    EXPECT_EQ(json["name"], "io.github.mrdevfx.ldm");
    EXPECT_EQ(json["type"], "stdio");
    EXPECT_EQ(json["path"], "C:\\Program Files\\LDM\\ldm-host.exe");
    EXPECT_EQ(json["allowed_origins"], nlohmann::json({"chrome-extension://jdkneeflbkgkkinfcpfamlckpoknbnln/"}));
    EXPECT_FALSE(json.contains("allowed_extensions"));
}

TEST(BrowserHostManifest, FirefoxManifestAllowsOnlyLdmAddOn)
{
    const auto json = nlohmann::json::parse(ldm::browser::NativeHostManifest(BrowserFamily::Firefox, L"C:\\LDM\\ldm-host.exe"));
    EXPECT_EQ(json["allowed_extensions"], nlohmann::json({"ldm-integration@mrdevfx.github.io"}));
    EXPECT_FALSE(json.contains("allowed_origins"));
}

}  // namespace
