#include "browser/HostManifest.h"
#include "browser/Protocol.h"
#include "model/Cookie.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace {

using ldm::browser::BrowserFamily;
using ldm::browser::HostInfo;

constexpr char kChromeId[] = "jdkneeflbkgkkinfcpfamlckpoknbnln";

std::string HelloJson(const std::string& extra = {})
{
    return R"({"v":1,"id":"7","type":"hello","browser":"Chrome","browserVersion":"141.0","extensionVersion":"1.0.0","incognitoAllowed":false,"startLdm":false)" +
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
        R"({"v":1,"id":"7","type":"hello","browser":"","browserVersion":"1","extensionVersion":"1","incognitoAllowed":true,"startLdm":false})"));
    EXPECT_FALSE(ldm::browser::ParseMessage(
        R"({"v":1,"id":"7","type":"hello","browser":"Chrome","browserVersion":"1","extensionVersion":"1","incognitoAllowed":"yes","startLdm":false})"));
    EXPECT_FALSE(ldm::browser::ParseMessage(R"({"v":1,"id":"7","type":"hello","browser":")" + std::string(65, 'x') +
                                            R"(","browserVersion":"1","extensionVersion":"1","incognitoAllowed":true,"startLdm":false})"));
    // Control characters in display text could forge lines in LDM's UI.
    EXPECT_FALSE(ldm::browser::ParseMessage(
        R"({"v":1,"id":"7","type":"hello","browser":"Chrome\n","browserVersion":"1","extensionVersion":"1","incognitoAllowed":true,"startLdm":false})"));
}

TEST(BrowserProtocol, HelloSaysWhetherToStartLdm)
{
    EXPECT_FALSE(ldm::browser::HelloAsksToStartLdm(HelloJson()));
    const std::string asking =
        R"({"v":1,"id":"7","type":"hello","browser":"Chrome","browserVersion":"1","extensionVersion":"1","incognitoAllowed":false,"startLdm":true})";
    EXPECT_TRUE(ldm::browser::HelloAsksToStartLdm(asking));
    EXPECT_TRUE(std::get<ldm::browser::Hello>(ldm::browser::ParseMessage(asking)->message).startLdm);
    EXPECT_FALSE(ldm::browser::HelloAsksToStartLdm(R"({"v":1,"id":"7","type":"hello","startLdm":true})"));
    EXPECT_FALSE(ldm::browser::ParseMessage(
        R"({"v":1,"id":"7","type":"hello","browser":"Chrome","browserVersion":"1","extensionVersion":"1","incognitoAllowed":false})"));
}

std::string CaptureJson(const std::string& fields)
{
    return R"({"v":1,"id":"c1","type":"capture","userAgent":"Mozilla/5.0",)" + fields + "}";
}

constexpr char kCookie[] =
    R"({"domain":"files.test","hostOnly":true,"path":"/","secure":true,"expires":0,"name":"sid","value":"abc"})";

TEST(BrowserProtocol, ParsesCapture)
{
    const auto parsed = ldm::browser::ParseMessage(CaptureJson(
        R"("url":"https://files.test/a.zip","finalUrl":"https://cdn.test/a.zip","referrer":"https://files.test/page",)"
        R"("fileName":"a.zip","fileSize":1234,"mimeType":"application/zip","cookies":[)" + std::string(kCookie) + "]"));
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->id, "c1");
    const auto& capture = std::get<ldm::browser::Capture>(parsed->message);
    EXPECT_EQ(capture.url, "https://files.test/a.zip");
    EXPECT_EQ(capture.finalUrl, "https://cdn.test/a.zip");
    EXPECT_EQ(capture.referrer, "https://files.test/page");
    EXPECT_EQ(capture.fileName, "a.zip");
    EXPECT_EQ(capture.fileSize, 1234);
    EXPECT_EQ(capture.userAgent, "Mozilla/5.0");
    ASSERT_EQ(capture.cookies.size(), 1u);
    EXPECT_EQ(capture.cookies[0].name, "sid");
    EXPECT_TRUE(capture.cookies[0].secure);
}

TEST(BrowserProtocol, CaptureNeedsOnlyAddressUserAgentAndCookies)
{
    const auto parsed = ldm::browser::ParseMessage(CaptureJson(R"("url":"HTTP://files.test/a.zip","cookies":[])"));
    ASSERT_TRUE(parsed);
    const auto& capture = std::get<ldm::browser::Capture>(parsed->message);
    EXPECT_FALSE(capture.finalUrl);
    EXPECT_FALSE(capture.fileSize);
    EXPECT_FALSE(capture.chosen) << "taken automatically unless the extension says otherwise";
}

TEST(BrowserProtocol, CaptureSaysWhenTheUserPickedLdm)
{
    const auto parsed = ldm::browser::ParseMessage(CaptureJson(R"("url":"https://a.test/x","chosen":true,"cookies":[])"));
    ASSERT_TRUE(parsed);
    EXPECT_TRUE(std::get<ldm::browser::Capture>(parsed->message).chosen);
    EXPECT_FALSE(ldm::browser::ParseMessage(CaptureJson(R"("url":"https://a.test/x","chosen":"yes","cookies":[])")));
}

TEST(BrowserProtocol, RejectsInvalidCaptures)
{
    // Addresses LDM cannot download, or that smuggle control characters.
    EXPECT_FALSE(ldm::browser::ParseMessage(CaptureJson(R"("url":"file:///C:/x.zip","cookies":[])")));
    EXPECT_FALSE(ldm::browser::ParseMessage(CaptureJson(R"("url":"javascript:alert","cookies":[])")));
    EXPECT_FALSE(ldm::browser::ParseMessage(CaptureJson(R"("url":"https://a.test/x\nHost: b","cookies":[])")));
    EXPECT_FALSE(ldm::browser::ParseMessage(CaptureJson(R"("url":"https://a.test/x","referrer":"data:x","cookies":[])")));
    // Sizes and names.
    EXPECT_FALSE(ldm::browser::ParseMessage(CaptureJson(R"("url":"https://a.test/x","fileSize":-1,"cookies":[])")));
    EXPECT_FALSE(ldm::browser::ParseMessage(CaptureJson(R"("url":"https://a.test/x","fileName":"a\tb","cookies":[])")));
    // Cookies are required and each must be well formed.
    EXPECT_FALSE(ldm::browser::ParseMessage(CaptureJson(R"("url":"https://a.test/x")")));
    EXPECT_FALSE(ldm::browser::ParseMessage(CaptureJson(
        R"("url":"https://a.test/x","cookies":[{"domain":"a.test","hostOnly":true,"path":"/","secure":false,"expires":0,"name":"n","value":"a\tb"}])")));
    EXPECT_FALSE(ldm::browser::ParseMessage(CaptureJson(
        R"("url":"https://a.test/x","cookies":[{"domain":".a.test","hostOnly":false,"path":"/","secure":false,"expires":0,"name":"n","value":"v"}])")));
    EXPECT_FALSE(ldm::browser::ParseMessage(CaptureJson(
        R"("url":"https://a.test/x","cookies":[{"domain":"a.test","hostOnly":true,"path":"x","secure":false,"expires":0,"name":"n","value":"v"}])")));
    // No user agent.
    EXPECT_FALSE(ldm::browser::ParseMessage(R"({"v":1,"id":"c","type":"capture","url":"https://a.test/x","cookies":[]})"));
}

TEST(BrowserProtocol, CaptureCarriesFormData)
{
    const auto parsed = ldm::browser::ParseMessage(CaptureJson(
        R"("url":"https://a.test/get","cookies":[],"post":{"contentType":"application/x-www-form-urlencoded","body":"dG9rZW49YWJj"})"));
    ASSERT_TRUE(parsed);
    const auto& capture = std::get<ldm::browser::Capture>(parsed->message);
    ASSERT_TRUE(capture.post);
    EXPECT_EQ(capture.post->contentType, "application/x-www-form-urlencoded");
    EXPECT_EQ(capture.post->body, "token=abc");
}

TEST(BrowserProtocol, RejectsUnsafeFormData)
{
    // A line break in the content type would add a request header.
    EXPECT_FALSE(ldm::browser::ParseMessage(CaptureJson(
        R"("url":"https://a.test/get","cookies":[],"post":{"contentType":"text/plain\r\nX-Evil: 1","body":"YQ=="})")));
    EXPECT_FALSE(ldm::browser::ParseMessage(CaptureJson(
        R"("url":"https://a.test/get","cookies":[],"post":{"contentType":"text/plain","body":"not base64!"})")));
    EXPECT_FALSE(ldm::browser::ParseMessage(CaptureJson(R"("url":"https://a.test/get","cookies":[],"post":"x")")));
}

TEST(BrowserProtocol, EncodesCaptureResult)
{
    const auto json = nlohmann::json::parse(ldm::browser::EncodeCaptureResult("c1", true));
    EXPECT_EQ(json["type"], "capture-result");
    EXPECT_EQ(json["id"], "c1");
    EXPECT_EQ(json["accepted"], true);
}

TEST(BrowserCookies, NetscapeLinesMarkHostOnlyAndSecure)
{
    const ldm::model::Cookie hostOnly{"a.test", true, "/", false, 0, "n", "v"};
    const ldm::model::Cookie domain{"a.test", false, "/dl", true, 1'900'000'000, "n", "v"};
    EXPECT_EQ(ldm::model::ToNetscapeLine(hostOnly), "a.test\tFALSE\t/\tFALSE\t0\tn\tv");
    EXPECT_EQ(ldm::model::ToNetscapeLine(domain), ".a.test\tTRUE\t/dl\tTRUE\t1900000000\tn\tv");
    const std::vector<ldm::model::Cookie> both{hostOnly, domain};
    EXPECT_EQ(ldm::model::CookieLines(ldm::model::CookiesToText(both)).size(), 2u);
}

TEST(BrowserCookies, ValidityRules)
{
    EXPECT_TRUE(ldm::model::IsValidCookie({"a.test", true, "/", false, 0, "n", ""}));
    EXPECT_FALSE(ldm::model::IsValidCookie({"", true, "/", false, 0, "n", "v"}));
    EXPECT_FALSE(ldm::model::IsValidCookie({"a test", true, "/", false, 0, "n", "v"}));
    EXPECT_TRUE(ldm::model::IsValidCookie({"fd00::1", true, "/", false, 0, "n", "v"}));
    EXPECT_FALSE(ldm::model::IsValidCookie({"fd00::1", false, "/", false, 0, "n", "v"})) << "an address has no subdomains";
    EXPECT_FALSE(ldm::model::IsValidCookie({"[fd00::1]", true, "/", false, 0, "n", "v"}));
    EXPECT_FALSE(ldm::model::IsValidCookie({"a.test", true, "/", false, 0, "", "v"}));
    EXPECT_FALSE(ldm::model::IsValidCookie({"a.test", true, "/", false, -1, "n", "v"}));
    EXPECT_FALSE(ldm::model::IsValidCookie({"a.test", true, "/", false, 0, "n", "v\r"}));
}

std::string LinksJson(const std::string& fields)
{
    return R"({"v":1,"id":"l1","type":"links","selection":false,"userAgent":"Mozilla/5.0",)" + fields + "}";
}

TEST(BrowserProtocol, ParsesLinks)
{
    const auto parsed = ldm::browser::ParseMessage(LinksJson(
        R"("referrer":"https://site.test/page","links":[)"
        R"({"url":"https://site.test/a.zip","text":"Caf\u00e9 \u2013 setup","kind":"link","fileName":"setup.zip"},)"
        R"({"url":"https://site.test/p.jpg","kind":"image"}],"cookies":[)" + std::string(kCookie) + "]"));
    ASSERT_TRUE(parsed);
    const auto& links = std::get<ldm::browser::Links>(parsed->message);
    EXPECT_FALSE(links.selection);
    EXPECT_EQ(links.referrer, "https://site.test/page");
    ASSERT_EQ(links.links.size(), 2u);
    EXPECT_EQ(links.links[0].text, "Caf\xC3\xA9 \xE2\x80\x93 setup");
    EXPECT_EQ(links.links[0].fileName, "setup.zip");
    EXPECT_EQ(links.links[0].kind, ldm::browser::LinkKind::Link);
    EXPECT_EQ(links.links[1].text, "");
    EXPECT_EQ(links.links[1].kind, ldm::browser::LinkKind::Image);
    EXPECT_EQ(links.cookies.size(), 1u);
}

TEST(BrowserProtocol, RejectsInvalidLinks)
{
    const auto one = [](const std::string& link) { return LinksJson(R"("links":[)" + link + R"(],"cookies":[])"); };
    EXPECT_TRUE(ldm::browser::ParseMessage(one(R"({"url":"https://a.test/x","kind":"text"})")));
    EXPECT_FALSE(ldm::browser::ParseMessage(LinksJson(R"("links":[],"cookies":[])"))) << "no links";
    EXPECT_FALSE(ldm::browser::ParseMessage(one(R"j({"url":"javascript:alert(1)","kind":"link"})j")));
    EXPECT_FALSE(ldm::browser::ParseMessage(one(R"({"url":"https://a.test/x","kind":"script"})")));
    EXPECT_FALSE(ldm::browser::ParseMessage(one(R"({"url":"https://a.test/x","kind":"link","text":"a\nb"})")));
    EXPECT_FALSE(ldm::browser::ParseMessage(one(R"({"url":"https://a.test/x","kind":"link","text":")" +
                                                std::string(ldm::browser::kMaxLinkText + 1, 'x') + R"("})")));
    EXPECT_FALSE(ldm::browser::ParseMessage(LinksJson(R"("links":[{"url":"https://a.test/x","kind":"link"}])")))
        << "cookies are required";
    std::string many;
    for (size_t i = 0; i <= ldm::browser::kMaxPageLinks; ++i) {
        many += std::string(i == 0 ? "" : ",") + R"({"url":"https://a.test/x","kind":"link"})";
    }
    EXPECT_FALSE(ldm::browser::ParseMessage(LinksJson(R"("links":[)" + many + R"(],"cookies":[])")));
}

TEST(BrowserProtocol, ParsesLinkInfo)
{
    const auto parsed = ldm::browser::ParseMessage(
        R"({"v":1,"id":"i1","type":"link-info","items":[{"index":0,"fileSize":5,"mimeType":"application/zip","fileName":"a.zip"},{"index":3,"failed":true}]})");
    ASSERT_TRUE(parsed);
    const auto& info = std::get<ldm::browser::LinkInfo>(parsed->message);
    ASSERT_EQ(info.items.size(), 2u);
    EXPECT_EQ(info.items[0].fileSize, 5);
    EXPECT_EQ(info.items[0].mimeType, "application/zip");
    EXPECT_EQ(info.items[0].fileName, "a.zip");
    EXPECT_FALSE(info.items[0].failed);
    EXPECT_EQ(info.items[1].index, 3u);
    EXPECT_TRUE(info.items[1].failed);
}

TEST(BrowserProtocol, RejectsInvalidLinkInfo)
{
    const auto item = [](const std::string& fields) {
        return R"({"v":1,"id":"i1","type":"link-info","items":[{)" + fields + "}]}";
    };
    EXPECT_FALSE(ldm::browser::ParseMessage(item(R"("index":-1)")));
    EXPECT_FALSE(ldm::browser::ParseMessage(item(R"("index":1.5)")));
    EXPECT_FALSE(ldm::browser::ParseMessage(item(R"("index":5000)")));
    EXPECT_FALSE(ldm::browser::ParseMessage(item(R"("index":0,"fileSize":-3)")));
    EXPECT_FALSE(ldm::browser::ParseMessage(item(R"("index":0,"mimeType":"text/html\r\nX: y")")));
    EXPECT_FALSE(ldm::browser::ParseMessage(item(R"("index":0,"failed":"yes")")));
    EXPECT_FALSE(ldm::browser::ParseMessage(R"({"v":1,"id":"i1","type":"link-info"})"));
}

TEST(BrowserProtocol, EncodesLinksAnswers)
{
    const auto result = nlohmann::json::parse(ldm::browser::EncodeLinksResult("l1", false));
    EXPECT_EQ(result["type"], "links-result");
    EXPECT_EQ(result["id"], "l1");
    EXPECT_EQ(result["accepted"], false);
    const auto done = nlohmann::json::parse(ldm::browser::EncodeLinksDone("l1"));
    EXPECT_EQ(done["type"], "links-done");
    EXPECT_EQ(done["id"], "l1");
}

TEST(BrowserCookies, AppliesByDomainPathAndScheme)
{
    const ldm::model::Cookie hostOnly{"a.test", true, "/", false, 0, "n", "v"};
    EXPECT_TRUE(ldm::model::CookieApplies(hostOnly, "https", "a.test", "/x"));
    EXPECT_FALSE(ldm::model::CookieApplies(hostOnly, "https", "sub.a.test", "/x"));
    const ldm::model::Cookie domain{"A.test", false, "/dl", true, 0, "n", "v"};
    EXPECT_TRUE(ldm::model::CookieApplies(domain, "https", "sub.a.test", "/dl"));
    EXPECT_TRUE(ldm::model::CookieApplies(domain, "https", "a.test", "/dl/f.zip"));
    EXPECT_FALSE(ldm::model::CookieApplies(domain, "https", "a.test", "/dlx")) << "path boundary";
    EXPECT_FALSE(ldm::model::CookieApplies(domain, "http", "a.test", "/dl")) << "secure needs https";
    EXPECT_FALSE(ldm::model::CookieApplies(domain, "https", "evila.test", "/dl")) << "not a subdomain";
    const ldm::model::Cookie slash{"a.test", true, "/dl/", false, 0, "n", "v"};
    EXPECT_TRUE(ldm::model::CookieApplies(slash, "http", "a.test", "/dl/f"));
    EXPECT_FALSE(ldm::model::CookieApplies(slash, "http", "a.test", "/dl"));
}

TEST(BrowserProtocol, RejectsUnknownTypes)
{
    EXPECT_FALSE(ldm::browser::ParseMessage(R"({"v":1,"id":"7","type":"launch-missiles"})"));
}

TEST(BrowserProtocol, HostInfoRoundTrips)
{
    const HostInfo chrome{BrowserFamily::Chromium, kChromeId};
    EXPECT_TRUE(ldm::browser::IsValidEnvelope(ldm::browser::EncodeHostError("ldm-elevated")));
    EXPECT_NE(ldm::browser::EncodeHostError("ldm-elevated").find(R"("type":"host-error")"), std::string::npos);
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
    EXPECT_EQ(json["menu"], nlohmann::json({{"downloadLink", true}, {"downloadAll", true}}));
    EXPECT_EQ(json["sitePages"], false) << "only while site videos are on";
}

TEST(BrowserProtocol, ParsesSitePages)
{
    const std::string json = R"({"v":1,"id":"s1","type":"site-page","tabId":12,"incognito":false,)"
                             R"("url":"https://www.youtube.com/watch?v=aqz-KE-bpKQ","pageTitle":"Bunny - YouTube",)"
                             R"("userAgent":"Mozilla/5.0"})";
    const auto parsed = ldm::browser::ParseMessage(json);
    ASSERT_TRUE(parsed);
    const auto* page = std::get_if<ldm::browser::SitePage>(&parsed->message);
    ASSERT_NE(page, nullptr);
    EXPECT_EQ(page->tabId, 12);
    EXPECT_EQ(page->url, "https://www.youtube.com/watch?v=aqz-KE-bpKQ");
    EXPECT_EQ(page->pageTitle, "Bunny - YouTube");
    EXPECT_EQ(page->userAgent, "Mozilla/5.0");

    const auto with = [&json](const std::string& from, const std::string& to) {
        std::string changed = json;
        changed.replace(changed.find(from), from.size(), to);
        return changed;
    };
    EXPECT_FALSE(ldm::browser::ParseMessage(with("https://www.youtube.com/watch?v=aqz-KE-bpKQ", "javascript:void")));
    EXPECT_FALSE(ldm::browser::ParseMessage(with(R"("tabId":12)", R"("tabId":-1)")));
    EXPECT_FALSE(ldm::browser::ParseMessage(with(R"("incognito":false,)", "")));
    EXPECT_FALSE(ldm::browser::ParseMessage(with(R"(,"userAgent":"Mozilla/5.0")", "")));
    EXPECT_FALSE(ldm::browser::ParseMessage(with("Bunny - YouTube", "Bunny\n")));
}

TEST(BrowserProtocol, ParsesSiteRequests)
{
    const std::string json = R"({"v":1,"id":"r1","type":"site-request","url":"https://www.dailymotion.com/video/x5kesuj",)"
                             R"("referrer":"https://news.example.test/","incognito":true,"pageTitle":"A page",)"
                             R"("userAgent":"Mozilla/5.0"})";
    const auto parsed = ldm::browser::ParseMessage(json);
    ASSERT_TRUE(parsed);
    const auto* request = std::get_if<ldm::browser::SiteRequest>(&parsed->message);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->url, "https://www.dailymotion.com/video/x5kesuj");
    EXPECT_EQ(request->referrer, "https://news.example.test/");
    EXPECT_TRUE(request->incognito);
    EXPECT_EQ(request->pageTitle, "A page");
    EXPECT_EQ(request->userAgent, "Mozilla/5.0");

    const auto with = [&json](const std::string& from, const std::string& to) {
        std::string changed = json;
        changed.replace(changed.find(from), from.size(), to);
        return changed;
    };
    EXPECT_TRUE(ldm::browser::ParseMessage(with(R"("referrer":"https://news.example.test/",)", "")));
    EXPECT_FALSE(ldm::browser::ParseMessage(with("https://www.dailymotion.com/video/x5kesuj", "file:///C:/x")));
    EXPECT_FALSE(ldm::browser::ParseMessage(with("https://news.example.test/", "javascript:void")));
    EXPECT_FALSE(ldm::browser::ParseMessage(with(R"("incognito":true,)", "")));
    EXPECT_FALSE(ldm::browser::ParseMessage(with(R"(,"userAgent":"Mozilla/5.0")", "")));
    EXPECT_FALSE(ldm::browser::ParseMessage(with("A page", "A\npage")));
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

std::string MediaJson(const std::string& fields)
{
    return R"({"v":1,"id":"m1","type":"media",)" + fields + "}";
}

constexpr char kMediaFields[] =
    R"("tabId":12,"frameId":0,"incognito":false,"kind":"hls","url":"https://cdn.a.test/master.m3u8",)"
    R"("pageUrl":"https://a.test/watch","pageTitle":"A film","mimeType":"application/vnd.apple.mpegurl",)"
    R"("userAgent":"Mozilla/5.0","cookies":[{"domain":"cdn.a.test","hostOnly":true,"path":"/","secure":true,"expires":0,"name":"s","value":"1"}])";

TEST(BrowserProtocol, ParsesMedia)
{
    const auto parsed = ldm::browser::ParseMessage(MediaJson(kMediaFields));
    ASSERT_TRUE(parsed);
    const auto& media = std::get<ldm::browser::MediaFound>(parsed->message);
    EXPECT_EQ(media.tabId, 12);
    EXPECT_EQ(media.frameId, 0);
    EXPECT_FALSE(media.incognito);
    EXPECT_EQ(media.kind, ldm::browser::MediaKind::Hls);
    EXPECT_EQ(media.url, "https://cdn.a.test/master.m3u8");
    EXPECT_EQ(media.pageUrl, "https://a.test/watch");
    EXPECT_EQ(media.pageTitle, "A film");
    EXPECT_EQ(media.mimeType, "application/vnd.apple.mpegurl");
    EXPECT_FALSE(media.fileSize);
    ASSERT_EQ(media.cookies.size(), 1u);

    const auto file = ldm::browser::ParseMessage(MediaJson(
        R"("tabId":3,"frameId":7,"incognito":true,"kind":"file","url":"http://a.test/v.mp4","fileSize":5000000,"userAgent":"UA","cookies":[])"));
    ASSERT_TRUE(file);
    const auto& plain = std::get<ldm::browser::MediaFound>(file->message);
    EXPECT_EQ(plain.kind, ldm::browser::MediaKind::File);
    EXPECT_EQ(plain.fileSize, 5'000'000);
    EXPECT_TRUE(plain.incognito);
    EXPECT_TRUE(plain.pageTitle.empty());
}

TEST(BrowserProtocol, RejectsInvalidMedia)
{
    const std::string good = kMediaFields;
    const auto with = [&good](const std::string& from, const std::string& to) {
        std::string fields = good;
        fields.replace(fields.find(from), from.size(), to);
        return MediaJson(fields);
    };
    EXPECT_FALSE(ldm::browser::ParseMessage(with(R"("tabId":12)", R"("tabId":-1)")));
    EXPECT_FALSE(ldm::browser::ParseMessage(with(R"("tabId":12)", R"("tabId":4294967296)")));
    EXPECT_FALSE(ldm::browser::ParseMessage(with(R"("tabId":12)", R"("tabId":"12")")));
    EXPECT_FALSE(ldm::browser::ParseMessage(with(R"("kind":"hls")", R"("kind":"rtmp")")));
    EXPECT_FALSE(ldm::browser::ParseMessage(with("https://cdn.a.test/master.m3u8", "ftp://cdn.a.test/master.m3u8")))
        << "media comes from the web only";
    EXPECT_FALSE(ldm::browser::ParseMessage(with("https://cdn.a.test/master.m3u8", "file:///C:/x.m3u8")));
    EXPECT_FALSE(ldm::browser::ParseMessage(with("https://a.test/watch", "javascript:alert(1)")));
    EXPECT_FALSE(ldm::browser::ParseMessage(with(R"("pageTitle":"A film")", R"("pageTitle":"A\nfilm")")));
    EXPECT_FALSE(ldm::browser::ParseMessage(with(R"("pageTitle":"A film")", R"("pageTitle":")" +
                                                                               std::string(1025, 'x') + "\"")));
    EXPECT_FALSE(ldm::browser::ParseMessage(with(R"("userAgent":"Mozilla/5.0",)", "")));
    EXPECT_FALSE(ldm::browser::ParseMessage(with(R"("incognito":false,)", "")));
}

std::string PositionJson(const std::string& override = {})
{
    std::string json =
        R"({"v":1,"id":"p1","type":"video-position","tabId":12,"visible":true,"fullscreen":false,)"
        R"("window":{"x":-8,"y":-8,"width":1936,"height":1048},"viewport":{"x":0,"y":0,"width":1920,"height":937},)"
        R"("viewportPositionKnown":false,"video":{"x":100.5,"y":80,"width":1280,"height":720},"pixelRatio":1.25})";
    if (!override.empty()) {
        const size_t colon = override.find(':');
        const std::string key = override.substr(0, colon + 1);
        const size_t start = json.find(key);
        size_t end = start + key.size();
        int depth = 0;
        for (; end < json.size(); ++end) {
            const char c = json[end];
            depth += c == '{' ? 1 : (c == '}' ? -1 : 0);
            if (depth < 0 || (depth == 0 && c == ',')) {
                break;
            }
        }
        json.replace(start, end - start, override);
    }
    return json;
}

TEST(BrowserProtocol, ParsesVideoPosition)
{
    const auto parsed = ldm::browser::ParseMessage(PositionJson());
    ASSERT_TRUE(parsed);
    const auto& position = std::get<ldm::browser::VideoPosition>(parsed->message);
    EXPECT_EQ(position.tabId, 12);
    EXPECT_TRUE(position.visible);
    EXPECT_FALSE(position.fullscreen);
    EXPECT_FALSE(position.viewportPositionKnown);
    EXPECT_DOUBLE_EQ(position.window.x, -8);
    EXPECT_DOUBLE_EQ(position.viewport.height, 937);
    EXPECT_DOUBLE_EQ(position.video.x, 100.5);
    EXPECT_DOUBLE_EQ(position.pixelRatio, 1.25);
}

TEST(BrowserProtocol, RejectsInvalidVideoPosition)
{
    for (const char* override : {R"("pixelRatio":0)", R"("pixelRatio":17)", R"("pixelRatio":"1")",
                                 R"("video":{"x":0,"y":0,"width":-1,"height":10})",
                                 R"("video":{"x":1e300,"y":0,"width":1,"height":1})",
                                 R"("video":{"x":0,"y":0,"width":1})", R"("window":null)", R"("visible":1)"}) {
        EXPECT_FALSE(ldm::browser::ParseMessage(PositionJson(override))) << override;
    }
}

TEST(BrowserProtocol, ParsesMediaReset)
{
    const auto parsed = ldm::browser::ParseMessage(R"({"v":1,"id":"r","type":"media-reset","tabId":5})");
    ASSERT_TRUE(parsed);
    EXPECT_EQ(std::get<ldm::browser::MediaReset>(parsed->message).tabId, 5);
    EXPECT_FALSE(ldm::browser::ParseMessage(R"({"v":1,"id":"r","type":"media-reset"})"));
}

TEST(BrowserProtocol, ParsesTheCookiesAnswer)
{
    const auto parsed = ldm::browser::ParseMessage(
        R"({"v":1,"id":"m9","type":"cookies","requestId":"cookies-1","cookies":[{"domain":"a.test","hostOnly":true,"path":"/","secure":false,"expires":0,"name":"sid","value":"x"}]})");
    ASSERT_TRUE(parsed);
    const auto* cookies = std::get_if<ldm::browser::Cookies>(&parsed->message);
    ASSERT_NE(cookies, nullptr);
    EXPECT_EQ(cookies->requestId, "cookies-1");
    ASSERT_EQ(cookies->cookies.size(), 1u);
    EXPECT_EQ(cookies->cookies[0].name, "sid");

    EXPECT_FALSE(ldm::browser::ParseMessage(R"({"v":1,"id":"m9","type":"cookies","cookies":[]})"));
    EXPECT_FALSE(ldm::browser::ParseMessage(R"({"v":1,"id":"m9","type":"cookies","requestId":"","cookies":[]})"));
    EXPECT_FALSE(ldm::browser::ParseMessage(R"({"v":1,"id":"m9","type":"cookies","requestId":"c","cookies":{}})"));
    EXPECT_FALSE(ldm::browser::ParseMessage(
        R"({"v":1,"id":"m9","type":"cookies","requestId":"c","cookies":[{"domain":"","name":"x","value":"y"}]})"));

    const auto request = nlohmann::json::parse(ldm::browser::EncodeCookiesRequest("cookies-2", "https://a.test/x"));
    EXPECT_EQ(request["v"], 1);
    EXPECT_EQ(request["id"], "cookies-2");
    EXPECT_EQ(request["type"], "cookies-request");
    EXPECT_EQ(request["url"], "https://a.test/x");
}

TEST(BrowserProtocol, EncodesMediaListAndMediaRules)
{
    const auto list = nlohmann::json::parse(ldm::browser::EncodeMediaList(9, true));
    EXPECT_EQ(list["type"], "media-list");
    EXPECT_EQ(list["tabId"], 9);
    EXPECT_EQ(list["hasMedia"], true);

    ldm::browser::Config config{"5.0.0", {}};
    config.media = {true, {"mp4", "webm"}, 1024 * 1024, {"*.example.test"}};
    const auto json = nlohmann::json::parse(ldm::browser::EncodeConfig(config, "7"));
    EXPECT_EQ(json["media"], nlohmann::json({{"enabled", true},
                                             {"types", {"mp4", "webm"}},
                                             {"minimumBytes", 1024 * 1024},
                                             {"siteExceptions", {"*.example.test"}}}));
}

}  // namespace
