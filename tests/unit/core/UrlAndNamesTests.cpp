#include "core/FileNames.h"
#include "core/Matching.h"
#include "core/Url.h"
#include "model/Settings.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using ldm::core::ExtractUrls;
using ldm::core::FileExtension;
using ldm::core::FileNameFromUrlPath;
using ldm::core::NumberedFileName;
using ldm::core::ParseUrl;
using ldm::core::SanitizeFileName;
using ldm::core::WildcardMatch;

TEST(Url, ParsesSupportedSchemes)
{
    const auto url = ParseUrl("  HTTPS://Example.COM:8443/files/a%20b.zip?x=1#frag  ");
    ASSERT_TRUE(url);
    EXPECT_EQ(url->scheme, "https");
    EXPECT_EQ(url->host, "example.com");
    EXPECT_EQ(url->port, 8443);
    EXPECT_EQ(url->path, "/files/a%20b.zip");
    EXPECT_EQ(url->query, "x=1");
    EXPECT_EQ(url->SiteRoot(), "https://example.com:8443/");

    const auto ftp = ParseUrl("ftp://user:pw@ftp.example.org");
    ASSERT_TRUE(ftp);
    EXPECT_EQ(ftp->path, "/");
    EXPECT_TRUE(ftp->hasUserInfo);
    EXPECT_EQ(ftp->host, "ftp.example.org");

    const auto v6 = ParseUrl("http://[::1]:8080/x");
    ASSERT_TRUE(v6);
    EXPECT_EQ(v6->host, "::1");
    EXPECT_EQ(v6->SiteRoot(), "http://[::1]:8080/");
}

TEST(Url, RejectsInvalidAddresses)
{
    for (const char* text : {"", "example.com/a.zip", "file:///C:/a.zip", "javascript:alert(1)", "http://", "http:///x",
                             "http://exa mple.com/", "http://host:0/", "http://host:70000/", "http://host:12a/",
                             "http://[::1/", "mailto:a@b.c", "http://..evil/", "http://a\tb/"}) {
        EXPECT_FALSE(ParseUrl(text)) << text;
    }
}

TEST(Url, ExtractsAddressesFromText)
{
    const auto urls = ExtractUrls(
        "Get http://a.test/one.zip, then (https://b.test/two.iso). Also ftp://c.test/three and again "
        "http://a.test/one.zip; not xhttp://d.test or mailto:e@f.test.");
    const std::vector<std::string> expected{"http://a.test/one.zip", "https://b.test/two.iso", "ftp://c.test/three"};
    EXPECT_EQ(urls, expected);
}

TEST(Url, QueryParametersAreAddedOrReplaceTheirNamesake)
{
    using ldm::core::WithQueryParameters;
    EXPECT_EQ(WithQueryParameters("https://a.test/seg1.ts", "Policy=p&Signature=s"), "https://a.test/seg1.ts?Policy=p&Signature=s");
    EXPECT_EQ(WithQueryParameters("https://a.test/seg1.ts?n=1&Signature=old", "Signature=s&Key-Pair-Id=k"),
              "https://a.test/seg1.ts?n=1&Signature=s&Key-Pair-Id=k");
    EXPECT_EQ(WithQueryParameters("https://a.test/seg1.ts?n=1#t=5", "a=b"), "https://a.test/seg1.ts?n=1&a=b#t=5");
    EXPECT_EQ(WithQueryParameters("https://a.test/seg1.ts?", "a=b"), "https://a.test/seg1.ts?a=b");
    EXPECT_EQ(WithQueryParameters("https://a.test/seg1.ts?n=1", ""), "https://a.test/seg1.ts?n=1");
}

TEST(FileNames, FromUrlPath)
{
    EXPECT_EQ(FileNameFromUrlPath("/dir/My%20File.zip"), "My File.zip");
    EXPECT_EQ(FileNameFromUrlPath("/dir/"), "");
    EXPECT_EQ(FileNameFromUrlPath("/a.zip?x=/b.exe"), "a.zip");
    EXPECT_EQ(FileNameFromUrlPath("/%E2%82%AC.txt"), "\xE2\x82\xAC.txt");
    EXPECT_EQ(FileNameFromUrlPath("/100%"), "100%");
}

TEST(FileNames, SanitizesForWindows)
{
    EXPECT_EQ(SanitizeFileName("..\\evil:name?.bin"), ".._evil_name_.bin");
    EXPECT_EQ(SanitizeFileName("a/b|c*d\"e<f>g"), "a_b_c_d_e_f_g");
    EXPECT_EQ(SanitizeFileName("trailing. . "), "trailing");
    EXPECT_EQ(SanitizeFileName("   "), "download");
    EXPECT_EQ(SanitizeFileName(".."), "download");
    EXPECT_EQ(SanitizeFileName("CON"), "_CON");
    EXPECT_EQ(SanitizeFileName("com1.txt"), "_com1.txt");
    EXPECT_EQ(SanitizeFileName("console.txt"), "console.txt");
    EXPECT_EQ(SanitizeFileName("tab\there\x01"), "tabhere");
    EXPECT_EQ(SanitizeFileName("bad\xFF.bin"), "bad\xEF\xBF\xBD.bin");  // invalid UTF-8 replaced

    const std::string longName = std::string(300, 'x') + ".iso";
    const std::string clipped = SanitizeFileName(longName);
    EXPECT_EQ(clipped.size(), 200u);
    EXPECT_TRUE(clipped.ends_with(".iso"));

    // The cut to 200 units must not leave a trailing space or dot either.
    const std::string spaced = std::string(199, 'y') + "  tail";
    EXPECT_EQ(SanitizeFileName(spaced), std::string(199, 'y'));
    EXPECT_EQ(SanitizeFileName(std::string(250, '.') + std::string(20, 'x')), "download");
    EXPECT_EQ(SanitizeFileName("CONOUT$.log"), "_CONOUT$.log");
}

TEST(FileNames, ExtensionAndNumbering)
{
    EXPECT_EQ(FileExtension("archive.TAR.GZ"), "gz");
    EXPECT_EQ(FileExtension(".hidden"), "");
    EXPECT_EQ(FileExtension("noext"), "");
    EXPECT_EQ(NumberedFileName("a.zip", 2), "a (2).zip");
    EXPECT_EQ(NumberedFileName("README", 3), "README (3)");
    EXPECT_EQ(NumberedFileName(".hidden", 2), ".hidden (2)");

    using ldm::core::SplitNumberedFileName;
    EXPECT_EQ(SplitNumberedFileName("range (2).bin").base, "range.bin");
    EXPECT_EQ(SplitNumberedFileName("range (2).bin").number, 2);
    EXPECT_EQ(SplitNumberedFileName("README (12)").base, "README");
    EXPECT_EQ(SplitNumberedFileName("README (12)").number, 12);
    EXPECT_EQ(SplitNumberedFileName("a.zip").number, 1);
    EXPECT_EQ(SplitNumberedFileName("Report (2019).pdf").number, 1);  // a year, not a counter
    EXPECT_EQ(SplitNumberedFileName("x (1).zip").number, 1);
    EXPECT_EQ(SplitNumberedFileName("x (02).zip").number, 1);
    EXPECT_EQ(SplitNumberedFileName("x(3).zip").number, 1);
    EXPECT_EQ(SplitNumberedFileName(" (3).zip").number, 1);
    EXPECT_EQ(SplitNumberedFileName("x ().zip").number, 1);
}

TEST(Matching, Wildcards)
{
    EXPECT_TRUE(WildcardMatch("*.example.com", "dl.EXAMPLE.com"));
    EXPECT_FALSE(WildcardMatch("*.example.com", "example.com"));
    EXPECT_TRUE(WildcardMatch("*", ""));
    EXPECT_TRUE(WildcardMatch("a*b*c", "aXXbYYc"));
    EXPECT_FALSE(WildcardMatch("a*b*c", "aXXbYY"));
    EXPECT_TRUE(WildcardMatch("127.0.0.1", "127.0.0.1"));
}

TEST(Matching, ExtensionLists)
{
    const std::string list = ldm::model::DefaultCapturedExtensions();
    EXPECT_TRUE(ldm::core::MatchesExtensionList(list, "zip"));
    EXPECT_TRUE(ldm::core::MatchesExtensionList(list, "ISO"));
    EXPECT_TRUE(ldm::core::MatchesExtensionList(list, "r01"));  // "R0*"
    EXPECT_FALSE(ldm::core::MatchesExtensionList(list, "html"));
    EXPECT_FALSE(ldm::core::MatchesExtensionList(list, ""));
    EXPECT_TRUE(ldm::core::MatchesExtensionList(".mp4 .mkv", "MKV"));
}

TEST(Matching, CategoryPrefersSitesThenExtensions)
{
    using ldm::model::Category;
    using ldm::model::CategoryId;
    std::vector<Category> categories(3);
    categories[0].id = CategoryId{1};
    categories[0].extensions = "zip rar 7z";
    categories[1].id = CategoryId{2};
    categories[1].extensions = "iso";
    categories[1].sites = "*.distro.test";
    categories[2].id = CategoryId{3};
    categories[2].extensions = ".MP3";
    const auto url = [](const char* text) { return *ParseUrl(text); };
    EXPECT_EQ(ldm::core::MatchCategory(categories, "a.ZIP", url("http://x.test/a.ZIP")), CategoryId{1});
    EXPECT_EQ(ldm::core::MatchCategory(categories, "a.zip", url("http://cdn.distro.test/a.zip")), CategoryId{2});
    EXPECT_EQ(ldm::core::MatchCategory(categories, "song.mp3", url("http://x.test/song.mp3")), CategoryId{3});
    EXPECT_FALSE(ldm::core::MatchCategory(categories, "doc.pdf", url("http://x.test/doc.pdf")));
}

TEST(Matching, ServerLimitAndSiteLogin)
{
    const std::vector<ldm::model::ServerLimit> limits{{"*.test", 4}, {"slow.example.test", 2}, {"*", 16}};
    EXPECT_EQ(ldm::core::MatchServerLimit(limits, "slow.example.test"), 2);
    EXPECT_EQ(ldm::core::MatchServerLimit(limits, "fast.test"), 4);
    EXPECT_EQ(ldm::core::MatchServerLimit(limits, "other.org"), 16);

    std::vector<ldm::model::SiteLogin> logins(3);
    logins[0].pattern = "files.test";
    logins[0].username = "site";
    logins[1].pattern = "files.test/private";
    logins[1].username = "private";
    logins[2].pattern = "*.mirror.test";
    logins[2].username = "mirror";
    const auto login = [&](const char* text) {
        const auto* match = ldm::core::MatchSiteLogin(logins, *ParseUrl(text));
        return match == nullptr ? std::string("-") : match->username;
    };
    EXPECT_EQ(login("http://files.test/public/a.zip"), "site");
    EXPECT_EQ(login("http://files.test/private/a.zip"), "private");
    EXPECT_EQ(login("http://files.test/privateer/a.zip"), "site");
    EXPECT_EQ(login("http://eu.mirror.test/a.zip"), "mirror");
    EXPECT_EQ(login("http://other.test/a.zip"), "-");
}

}  // namespace
