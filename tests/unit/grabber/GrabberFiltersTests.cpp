#include "grabber/GrabberFilters.h"

#include <gtest/gtest.h>

#include <string_view>

namespace {

using ldm::core::ParseUrl;
using ldm::core::Url;
using ldm::grabber::BaseSite;
using ldm::grabber::FileMatch;
using ldm::grabber::PageDepth;

Url U(std::string_view text)
{
    const auto url = ParseUrl(text);
    EXPECT_TRUE(url) << text;
    return url.value_or(Url{});
}

TEST(GrabberFilters, FindsTheMainDomain)
{
    using ldm::grabber::MainDomainOf;
    EXPECT_EQ(MainDomainOf("example.com"), "example.com");
    EXPECT_EQ(MainDomainOf("www.example.com"), "example.com");
    EXPECT_EQ(MainDomainOf("a.b.example.com"), "example.com");
    EXPECT_EQ(MainDomainOf("www.a.example.co.uk"), "example.co.uk");
    EXPECT_EQ(MainDomainOf("shop.example.com.au"), "example.com.au");
    EXPECT_EQ(MainDomainOf("localhost"), "localhost");
    EXPECT_EQ(MainDomainOf("192.168.1.10"), "192.168.1.10");
    EXPECT_EQ(MainDomainOf("::1"), "::1");
}

TEST(GrabberFilters, JudgesTheBaseSite)
{
    const BaseSite site = ldm::grabber::BaseSiteOf(U("https://www.example.com/docs/index.html"));
    EXPECT_EQ(site.host, "www.example.com");
    EXPECT_EQ(site.mainDomain, "example.com");
    EXPECT_EQ(site.folder, "/docs/");

    EXPECT_TRUE(ldm::grabber::OnBaseSite(site, U("http://www.example.com/x"), false));
    EXPECT_FALSE(ldm::grabber::OnBaseSite(site, U("http://cdn.example.com/x"), false));
    EXPECT_TRUE(ldm::grabber::OnBaseSite(site, U("http://cdn.example.com/x"), true));
    EXPECT_TRUE(ldm::grabber::OnBaseSite(site, U("http://example.com/x"), true));
    EXPECT_FALSE(ldm::grabber::OnBaseSite(site, U("http://notexample.com/x"), true));
    EXPECT_FALSE(ldm::grabber::OnBaseSite(site, U("http://example.com.evil.net/x"), true));
}

TEST(GrabberFilters, CountsDepths)
{
    const PageDepth start{};
    const PageDepth onBase = ldm::grabber::DepthOfLink(start, true);
    EXPECT_EQ(onBase, (PageDepth{1, 0}));
    const PageDepth other = ldm::grabber::DepthOfLink(onBase, false);
    EXPECT_EQ(other, (PageDepth{2, 1}));
    EXPECT_EQ(ldm::grabber::DepthOfLink(other, false), (PageDepth{3, 2}));
    EXPECT_EQ(ldm::grabber::DepthOfLink(other, true), (PageDepth{3, 0}));
}

TEST(GrabberFilters, ExploresWithinTheDepths)
{
    const BaseSite site = ldm::grabber::BaseSiteOf(U("http://example.com/docs/"));
    ldm::model::GrabberExploreFilters filters;
    filters.baseSiteDepth = 1;
    filters.otherSitesDepth = 0;
    EXPECT_TRUE(ldm::grabber::ShouldExplore(filters, site, U("http://example.com/docs/a.html"), {1, 0}));
    EXPECT_FALSE(ldm::grabber::ShouldExplore(filters, site, U("http://example.com/docs/b.html"), {2, 0}));
    EXPECT_FALSE(ldm::grabber::ShouldExplore(filters, site, U("http://other.com/docs/"), {1, 1}));

    filters.otherSitesDepth = 1;
    EXPECT_TRUE(ldm::grabber::ShouldExplore(filters, site, U("http://other.com/docs/"), {1, 1}));
    EXPECT_FALSE(ldm::grabber::ShouldExplore(filters, site, U("http://other.com/more/"), {2, 2}));

    filters.baseSiteDepth = std::nullopt;  // the whole site
    EXPECT_TRUE(ldm::grabber::ShouldExplore(filters, site, U("http://example.com/docs/deep.html"), {40, 0}));
}

TEST(GrabberFilters, KeepsBelowTheStartFolder)
{
    const BaseSite site = ldm::grabber::BaseSiteOf(U("http://example.com/docs/start.html"));
    ldm::model::GrabberExploreFilters filters;
    filters.baseSiteDepth = 5;
    EXPECT_TRUE(ldm::grabber::ShouldExplore(filters, site, U("http://example.com/docs/sub/a.html"), {1, 0}));
    EXPECT_TRUE(ldm::grabber::ShouldExplore(filters, site, U("http://example.com/DOCS/a.html"), {1, 0}));
    EXPECT_FALSE(ldm::grabber::ShouldExplore(filters, site, U("http://example.com/a.html"), {1, 0}));
    EXPECT_FALSE(ldm::grabber::ShouldExplore(filters, site, U("http://example.com/docsx/a.html"), {1, 0}));
    filters.noParentDirectories = false;
    EXPECT_TRUE(ldm::grabber::ShouldExplore(filters, site, U("http://example.com/a.html"), {1, 0}));
}

TEST(GrabberFilters, AppliesPathMasksToPages)
{
    const BaseSite site = ldm::grabber::BaseSiteOf(U("http://example.com/"));
    ldm::model::GrabberExploreFilters filters;
    filters.baseSiteDepth = 5;
    filters.includePaths = {"/docs/", "*.php"};
    filters.excludePaths = {"/docs/private/*", "/logout"};
    EXPECT_TRUE(ldm::grabber::ShouldExplore(filters, site, U("http://example.com/docs/a.html"), {1, 0}));
    EXPECT_TRUE(ldm::grabber::ShouldExplore(filters, site, U("http://example.com/x/index.PHP"), {1, 0}));
    EXPECT_FALSE(ldm::grabber::ShouldExplore(filters, site, U("http://example.com/blog/a.html"), {1, 0}));
    EXPECT_FALSE(ldm::grabber::ShouldExplore(filters, site, U("http://example.com/docs/private/a.html"), {1, 0}));
    EXPECT_FALSE(ldm::grabber::ShouldExplore(filters, site, U("http://example.com/logout.php"), {1, 0}));
}

TEST(GrabberFilters, MatchesFiles)
{
    const BaseSite site = ldm::grabber::BaseSiteOf(U("http://example.com/"));
    ldm::model::GrabberFileFilters files;
    files.includeTypes = {"*.jpg", "*.png"};
    files.excludeTypes = {"thumb_*"};
    EXPECT_EQ(ldm::grabber::MatchFile(files, site, false, U("http://example.com/a/B.JPG"), std::nullopt),
              FileMatch::Match);
    EXPECT_EQ(ldm::grabber::MatchFile(files, site, false, U("http://example.com/a/b.gif"), std::nullopt),
              FileMatch::Filtered);
    EXPECT_EQ(ldm::grabber::MatchFile(files, site, false, U("http://example.com/a/thumb_b.png"), std::nullopt),
              FileMatch::Filtered);
    EXPECT_EQ(ldm::grabber::MatchFile(files, site, false, U("http://example.com/a/b%20c.png"), std::nullopt),
              FileMatch::Match);

    files.thisSiteOnly = true;
    EXPECT_EQ(ldm::grabber::MatchFile(files, site, false, U("http://cdn.example.com/b.png"), std::nullopt),
              FileMatch::Filtered);
    EXPECT_EQ(ldm::grabber::MatchFile(files, site, true, U("http://cdn.example.com/b.png"), std::nullopt),
              FileMatch::Match);

    files.excludePaths = {"/private/"};
    EXPECT_EQ(ldm::grabber::MatchFile(files, site, false, U("http://example.com/private/b.png"), std::nullopt),
              FileMatch::Filtered);
}

TEST(GrabberFilters, AppliesSizeLimitsAtTheBoundaries)
{
    const BaseSite site = ldm::grabber::BaseSiteOf(U("http://example.com/"));
    ldm::model::GrabberFileFilters files;
    files.minBytes = 100;
    files.maxBytes = 200;
    const Url url = U("http://example.com/f.bin");
    EXPECT_EQ(ldm::grabber::MatchFile(files, site, false, url, std::nullopt), FileMatch::NeedsSize);
    EXPECT_EQ(ldm::grabber::MatchFile(files, site, false, url, 99), FileMatch::Filtered);
    EXPECT_EQ(ldm::grabber::MatchFile(files, site, false, url, 100), FileMatch::Match);
    EXPECT_EQ(ldm::grabber::MatchFile(files, site, false, url, 200), FileMatch::Match);
    EXPECT_EQ(ldm::grabber::MatchFile(files, site, false, url, 201), FileMatch::Filtered);
    files.includeTypes = {"*.zip"};
    EXPECT_EQ(ldm::grabber::MatchFile(files, site, false, url, std::nullopt), FileMatch::Filtered);
}

TEST(GrabberFilters, NormalizesAddressesForDuplicates)
{
    using ldm::grabber::NormalizeAddress;
    EXPECT_EQ(NormalizeAddress(U("HTTP://Example.COM:80/a/b.html?x=1#frag")), "http://example.com/a/b.html?x=1");
    EXPECT_EQ(NormalizeAddress(U("https://example.com:443/")), "https://example.com/");
    EXPECT_EQ(NormalizeAddress(U("https://example.com:8443")), "https://example.com:8443/");
    EXPECT_EQ(NormalizeAddress(U("http://[::1]:8080/a")), "http://[::1]:8080/a");
    EXPECT_NE(NormalizeAddress(U("http://example.com/A")), NormalizeAddress(U("http://example.com/a")));
}

TEST(GrabberFilters, GuessesPagesByExtension)
{
    using ldm::grabber::LooksLikePage;
    EXPECT_TRUE(LooksLikePage(U("http://example.com/")));
    EXPECT_TRUE(LooksLikePage(U("http://example.com/docs/")));
    EXPECT_TRUE(LooksLikePage(U("http://example.com/about")));
    EXPECT_TRUE(LooksLikePage(U("http://example.com/a.HTML")));
    EXPECT_TRUE(LooksLikePage(U("http://example.com/view.php?id=3")));
    EXPECT_FALSE(LooksLikePage(U("http://example.com/a.zip")));
    EXPECT_FALSE(LooksLikePage(U("http://example.com/a.jpg?w=100")));
}

TEST(GrabberFilters, RecognisesPageTypes)
{
    using ldm::grabber::IsPageType;
    EXPECT_TRUE(IsPageType("text/html"));
    EXPECT_TRUE(IsPageType("Text/HTML; charset=utf-8"));
    EXPECT_TRUE(IsPageType(" application/xhtml+xml "));
    EXPECT_FALSE(IsPageType("text/plain"));
    EXPECT_FALSE(IsPageType("application/octet-stream"));
    EXPECT_FALSE(IsPageType(""));
}

}  // namespace
