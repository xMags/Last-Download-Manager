#include "core/LaunchRequest.h"
#include "platform/win/InstanceChannel.h"
#include "model/Settings.h"
#include "ui/ClipboardMonitor.h"
#include "ui/ExportFormat.h"

#include <gtest/gtest.h>

#include <memory>

namespace {

using ldm::core::ParseCommandLine;

std::unique_ptr<ldm::core::DownloadView> View(std::string url, std::string name, std::string description = "")
{
    auto view = std::make_unique<ldm::core::DownloadView>();
    view->download.url = std::move(url);
    view->download.fileName = std::move(name);
    view->download.description = std::move(description);
    return view;
}

TEST(ExportFormat, RoundTripsAddressesNamesAndDescriptions)
{
    const auto a = View("https://x.test/a.zip", "a.zip", "first");
    const auto b = View("ftp://y.test/b.iso", "b (2).iso");
    const std::vector<const ldm::core::DownloadView*> views{a.get(), b.get()};
    const auto parsed = ldm::ui::ParseLdmExport(ldm::ui::ExportLdm(views));
    ASSERT_FALSE(parsed.error);
    ASSERT_EQ(parsed.downloads.size(), 2u);
    EXPECT_EQ(parsed.downloads[0].url, "https://x.test/a.zip");
    EXPECT_EQ(parsed.downloads[0].description, "first");
    EXPECT_EQ(parsed.downloads[1].fileName, "b (2).iso");
    EXPECT_EQ(ldm::ui::ExportText(views), "https://x.test/a.zip\r\nftp://y.test/b.iso\r\n");
}

TEST(ExportFormat, RejectsMalformedFilesWhole)
{
    for (const char* text : {
             "", "not json", "[]", R"({"format":"other","version":1,"downloads":[]})",
             R"({"format":"ldm-export","version":2,"downloads":[]})",
             R"({"format":"ldm-export","version":1,"downloads":{}})",
             R"({"format":"ldm-export","version":1,"downloads":[{"url":"file:///C:/x"}]})",
             R"({"format":"ldm-export","version":1,"downloads":[{"url":"http://x.test/a","fileName":"..\\evil"}]})",
             R"({"format":"ldm-export","version":1,"downloads":[{"url":"http://x.test/a"},{"url":42}]})",
         }) {
        const auto parsed = ldm::ui::ParseLdmExport(text);
        EXPECT_TRUE(parsed.error) << text;
        EXPECT_TRUE(parsed.downloads.empty()) << text;
    }
}

TEST(CommandLine, ParsesSwitches)
{
    const auto parse = ParseCommandLine({L"https://x.test/a.zip", L"--save-dir", L"D:\\Files", L"--name", L"b.zip", L"--silent"});
    ASSERT_FALSE(parse.error);
    EXPECT_EQ(parse.request.url, "https://x.test/a.zip");
    EXPECT_EQ(parse.request.saveDir, "D:\\Files");
    EXPECT_EQ(parse.request.fileName, "b.zip");
    EXPECT_TRUE(parse.request.silent);
    EXPECT_FALSE(parse.request.queueOnly);

    EXPECT_TRUE(ParseCommandLine({L"--tray"}).request.tray);
    EXPECT_TRUE(ParseCommandLine({L"--help"}).request.help);
    EXPECT_FALSE(ParseCommandLine({}).error);
}

TEST(CommandLine, RejectsBadInput)
{
    for (const auto& arguments : std::vector<std::vector<std::wstring>>{
             {L"notaurl"},
             {L"https://x.test/a", L"https://x.test/b"},
             {L"https://x.test/a", L"--save-dir", L"relative\\dir"},
             {L"https://x.test/a", L"--save-dir"},
             {L"https://x.test/a", L"--name", L"..\\up.zip"},
             {L"--silent"},
             {L"https://x.test/a", L"--unknown"},
         }) {
        EXPECT_TRUE(ParseCommandLine(arguments).error) << arguments.size();
    }
}

TEST(ClipboardMonitor, OffersOnlyALoneAddressWithACapturedExtension)
{
    const std::string captured = ldm::model::DefaultCapturedExtensions();
    EXPECT_EQ(ldm::ui::OfferableAddress("  https://x.test/files/a.zip\r\n", captured), "https://x.test/files/a.zip");
    EXPECT_EQ(ldm::ui::OfferableAddress("https://x.test/a.ISO?token=1", captured), "https://x.test/a.ISO?token=1");
    EXPECT_FALSE(ldm::ui::OfferableAddress("https://x.test/page.html", captured));
    EXPECT_FALSE(ldm::ui::OfferableAddress("https://x.test/", captured));
    EXPECT_FALSE(ldm::ui::OfferableAddress("see https://x.test/a.zip", captured));
    EXPECT_FALSE(ldm::ui::OfferableAddress("https://x.test/a.zip https://x.test/b.zip", captured));
    EXPECT_FALSE(ldm::ui::OfferableAddress("file:///C:/a.zip", captured));
    EXPECT_FALSE(ldm::ui::OfferableAddress("", captured));
}

TEST(InstanceChannel, LaunchRequestRoundTripAndValidation)
{
    ldm::core::LaunchRequest request;
    request.url = "https://x.test/a.zip";
    request.saveDir = "C:\\Downloads";
    request.fileName = "a.zip";
    request.queueOnly = true;
    const auto decoded = ldm::platform::DecodeLaunchRequest(ldm::platform::EncodeLaunchRequest(request, "17"));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->url, request.url);
    EXPECT_EQ(decoded->saveDir, request.saveDir);
    EXPECT_EQ(decoded->fileName, request.fileName);
    EXPECT_TRUE(decoded->queueOnly);

    for (const char* bad : {
             R"j({"protocol":1,"id":"1","type":"launch","url":"javascript:alert(1)"})j",
             R"({"protocol":1,"id":"1","type":"launch","saveDir":"relative"})",
             R"({"protocol":1,"id":"1","type":"launch","fileName":"a/b"})",
             R"({"protocol":2,"id":"1","type":"launch"})",
             R"({"protocol":1,"id":"1","type":"other"})",
             R"({"protocol":1,"type":"launch"})",
             R"({"protocol":1,"id":"1","type":"launch","silent":"yes"})",
             "garbage",
         }) {
        EXPECT_FALSE(ldm::platform::DecodeLaunchRequest(bad)) << bad;
    }
}

}  // namespace
