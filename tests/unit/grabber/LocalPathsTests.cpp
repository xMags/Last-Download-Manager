#include "grabber/LocalPaths.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>

namespace {

using ldm::grabber::AttributeSafeAddress;
using ldm::grabber::LocalRelativePath;
using ldm::grabber::RelativeReference;

ldm::core::Url U(std::string_view text)
{
    const auto url = ldm::core::ParseUrl(text);
    EXPECT_TRUE(url) << text;
    return url.value_or(ldm::core::Url{});
}

TEST(LocalPaths, NamesPagesAndFiles)
{
    EXPECT_EQ(LocalRelativePath(U("http://example.com/"), true, true), "example.com/index.html");
    EXPECT_EQ(LocalRelativePath(U("http://example.com/docs/"), true, true), "example.com/docs/index.html");
    EXPECT_EQ(LocalRelativePath(U("http://example.com/docs/a.html"), true, true), "example.com/docs/a.html");
    EXPECT_EQ(LocalRelativePath(U("http://example.com/docs/about"), true, true), "example.com/docs/about.html");
    EXPECT_EQ(LocalRelativePath(U("http://example.com/view.php"), true, false), "view.php.html");
    EXPECT_EQ(LocalRelativePath(U("http://example.com/img/a%20b.jpg"), false, true), "example.com/img/a b.jpg");
    EXPECT_EQ(LocalRelativePath(U("http://example.com/img/a.jpg"), false, false), "a.jpg");
    EXPECT_EQ(LocalRelativePath(U("http://example.com:8080/a.jpg"), false, true), "example.com_8080/a.jpg");
    EXPECT_EQ(LocalRelativePath(U("http://example.com/dir/"), false, false), "index");
}

TEST(LocalPaths, KeepsQueriesApart)
{
    const std::string one = LocalRelativePath(U("http://example.com/view.php?id=1"), true, false);
    const std::string two = LocalRelativePath(U("http://example.com/view.php?id=2"), true, false);
    EXPECT_NE(one, two);
    EXPECT_TRUE(one.starts_with("view.php_")) << one;
    EXPECT_TRUE(one.ends_with(".html")) << one;
    EXPECT_EQ(one, LocalRelativePath(U("http://example.com/view.php?id=1"), true, false));
}

TEST(LocalPaths, MakesUnsafeNamesSafe)
{
    const std::string path = LocalRelativePath(U("http://example.com/a%3Ab/c%2Fd%5C..%2Fe.txt"), false, true);
    EXPECT_EQ(path.find(':'), std::string::npos) << path;
    EXPECT_EQ(path.find('\\'), std::string::npos) << path;
    EXPECT_EQ(std::count(path.begin(), path.end(), '/'), 2) << path;  // host, one folder, the name
}

TEST(LocalPaths, BuildsRelativeReferences)
{
    EXPECT_EQ(RelativeReference("example.com/index.html", "example.com/img/a.jpg"), "img/a.jpg");
    EXPECT_EQ(RelativeReference("example.com/docs/a.html", "example.com/img/a.jpg"), "../img/a.jpg");
    EXPECT_EQ(RelativeReference("example.com/a/b/c.html", "cdn.com/x.css"), "../../../cdn.com/x.css");
    EXPECT_EQ(RelativeReference("example.com/a.html", "example.com/a b,c&d\".jpg"), "a%20b%2Cc%26d%22.jpg");
    EXPECT_EQ(RelativeReference("index.html", "a.jpg"), "a.jpg");
    EXPECT_EQ(RelativeReference("example.com/a.html", "example.com/\xC3\xA9.jpg"), "%C3%A9.jpg");
}

TEST(LocalPaths, MakesAddressesSafeInAttributes)
{
    EXPECT_EQ(AttributeSafeAddress("http://example.com/a b?x=1&y=\"2\",<3>"),
              "http://example.com/a%20b?x=1&amp;y=%222%22%2C%3C3%3E");
}

}  // namespace
