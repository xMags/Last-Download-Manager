#include "grabber/HtmlLinks.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace {

using ldm::grabber::DecodeEntities;
using ldm::grabber::LinkKind;
using ldm::grabber::LinkReplacement;
using ldm::grabber::PageLinks;
using ldm::grabber::RewriteLinks;
using ldm::grabber::ScanLinks;

std::vector<std::string> Addresses(const PageLinks& links)
{
    std::vector<std::string> addresses;
    for (const auto& link : links.links) {
        addresses.push_back(link.address);
    }
    return addresses;
}

TEST(HtmlLinks, FindsEveryLinkingAttribute)
{
    const std::string_view html = R"(<html><body background="bg.png">
<a href="page.html">Page</a> <area href="map.html" alt="Map">
<img src="a.jpg" srcset="b.jpg 1x, c.jpg 2x" alt="Pic"><picture><source srcset="d.webp"></picture>
<video src="v.mp4" poster="p.jpg"></video><audio src="s.mp3"></audio><track src="t.vtt">
<embed src="e.swf"><object data="o.pdf"></object><input type="IMAGE" src="i.gif"><input type="text" src="no.gif">
<link rel="stylesheet" href="s.css"><link rel="next" href="next.html"><link rel="shortcut icon" href="f.ico">
<frame src="f.html"><iframe src="if.html"></iframe><form action="search"></form>
<meta http-equiv="Refresh" content="5; URL='later.html'"><script src="x.js"></script>
<table background="t.png"><td background="td.png"></td></table></body></html>)";
    const PageLinks links = ScanLinks(html, 1000);
    const std::vector<std::string> expected{"bg.png",    "page.html", "map.html", "a.jpg",    "b.jpg",  "c.jpg",
                                            "d.webp",    "v.mp4",     "p.jpg",    "s.mp3",    "t.vtt",  "e.swf",
                                            "o.pdf",     "i.gif",     "s.css",    "next.html", "f.ico", "f.html",
                                            "if.html",   "search",    "later.html", "x.js",   "t.png",  "td.png"};
    EXPECT_EQ(Addresses(links), expected);
    EXPECT_FALSE(links.truncated);

    auto kindOf = [&links](std::string_view address) {
        for (const auto& link : links.links) {
            if (link.address == address) {
                return link.kind;
            }
        }
        ADD_FAILURE() << address;
        return LinkKind::Anchor;
    };
    EXPECT_EQ(kindOf("page.html"), LinkKind::Anchor);
    EXPECT_EQ(kindOf("next.html"), LinkKind::Anchor);
    EXPECT_EQ(kindOf("s.css"), LinkKind::Embedded);
    EXPECT_EQ(kindOf("f.ico"), LinkKind::Embedded);
    EXPECT_EQ(kindOf("a.jpg"), LinkKind::Embedded);
    EXPECT_EQ(kindOf("if.html"), LinkKind::Frame);
    EXPECT_EQ(kindOf("later.html"), LinkKind::Refresh);
    EXPECT_EQ(kindOf("search"), LinkKind::Form);
}

TEST(HtmlLinks, RecordsLinkText)
{
    const PageLinks links = ScanLinks(
        "<a href=one.html>  First\n\t <b>link</b>  </a><a href=two.html><img src=t.png alt='Second &amp; more'></a>"
        "<a href=three.html>unclosed<a href=four.html>Four</a><img src=x.png alt=\"Picture\">",
        100);
    ASSERT_EQ(links.links.size(), 6u);
    EXPECT_EQ(links.links[0].text, "First link");
    EXPECT_EQ(links.links[1].text, "Second & more");  // the image's alt stands for an anchor without text
    EXPECT_EQ(links.links[2].text, "Second & more");
    EXPECT_EQ(links.links[3].text, "unclosed");
    EXPECT_EQ(links.links[4].text, "Four");
    EXPECT_EQ(links.links[5].text, "Picture");
}

TEST(HtmlLinks, CutsLongTextOnACharacterBoundary)
{
    std::string html = "<a href=x.html>";
    for (int i = 0; i < 200; ++i) {
        html += "\xC3\xA9";  // é, two bytes
    }
    html += "</a>";
    const PageLinks links = ScanLinks(html, 10);
    ASSERT_EQ(links.links.size(), 1u);
    EXPECT_LE(links.links[0].text.size(), ldm::grabber::kMaxLinkText);
    EXPECT_EQ(links.links[0].text.size() % 2, 0u);

    // Text that fills the limit with whole characters keeps all of them.
    std::string exact = "<a href=y.html>";
    for (size_t i = 0; i < ldm::grabber::kMaxLinkText / 2; ++i) {
        exact += "\xC3\xA9";
    }
    exact += "x</a>";
    const PageLinks full = ScanLinks(exact, 10);
    ASSERT_EQ(full.links.size(), 1u);
    EXPECT_EQ(full.links[0].text.size(), ldm::grabber::kMaxLinkText);
}

TEST(HtmlLinks, SkipsCommentsScriptsAndStyles)
{
    const PageLinks links = ScanLinks(R"(<!-- <a href="commented.html"> -->
<script>var s = '<a href="script.html">';</script><SCRIPT type="x">document.write("<img src=w.png>")</SCRIPT >
<style>a { background: url(style.png) } </style><textarea><a href="text.html"></textarea>
<!DOCTYPE html><?xml version="1.0"?><a href="real.html">Real</a>)",
                                       100);
    EXPECT_EQ(Addresses(links), std::vector<std::string>{"real.html"});
}

TEST(HtmlLinks, DropsLinksThatAreNotAddresses)
{
    const PageLinks links = ScanLinks(R"x(<a href="javascript:void(0)">J</a><a href="  MAILTO:me@example.com">M</a>
<a href="#top">T</a><img src="data:image/png;base64,AAAA"><a href="tel:123">P</a><a href="">E</a><a>N</a>
<a href="ok.html">ok</a>)x",
                                       100);
    EXPECT_EQ(Addresses(links), std::vector<std::string>{"ok.html"});
}

TEST(HtmlLinks, DecodesEntitiesAndReadsBase)
{
    const PageLinks links = ScanLinks(
        R"(<base href="http://example.com/a/&amp;b/"><base href="ignored/"><a href="x.php?a=1&amp;b=2&lt=3&#38;c=&#x34;">X</a>)",
        100);
    ASSERT_TRUE(links.baseHref);
    EXPECT_EQ(*links.baseHref, "http://example.com/a/&b/");
    EXPECT_EQ(Addresses(links), std::vector<std::string>{"x.php?a=1&b=2&lt=3&c=4"});
}

TEST(HtmlLinks, ReadsEmptyCommentsAsBrowsersDo)
{
    const PageLinks links = ScanLinks(R"(<!--><a href="a.zip">A</a><!---><a href="b.zip">B</a><!-- <a href="c.zip"> -->)", 100);
    EXPECT_EQ(Addresses(links), (std::vector<std::string>{"a.zip", "b.zip"}));
}

TEST(HtmlLinks, RemovesLineBreaksAndTabsInsideAddresses)
{
    const PageLinks links = ScanLinks("<a href=\"/files/annual-\r\nreport.pdf\">R</a><a href=\"a&#10;b\tc.pdf\">X</a>", 100);
    EXPECT_EQ(Addresses(links), (std::vector<std::string>{"/files/annual-report.pdf", "abc.pdf"}));
}

TEST(HtmlLinks, FindsTheBaseHrefForRewriting)
{
    const std::string html = R"(<base href=" https://example.com/ "><img src="b.png">)";
    const PageLinks links = ScanLinks(html, 100);
    ASSERT_TRUE(links.baseHrefRange);
    EXPECT_EQ(html.substr(links.baseHrefRange->begin, links.baseHrefRange->end - links.baseHrefRange->begin),
              "https://example.com/");
}

TEST(HtmlLinks, DecodesEntities)
{
    EXPECT_EQ(DecodeEntities("a &amp; b &lt;c&gt; &quot;d&quot; &#233; &#xE9; &unknown; &"), "a & b <c> \"d\" \xC3\xA9 \xC3\xA9 &unknown; &");
    EXPECT_EQ(DecodeEntities("&#0;&#xD800;&#x110000;"), "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
    EXPECT_EQ(DecodeEntities("&nbsp;"), " ");
    EXPECT_EQ(DecodeEntities("&#0000000065;&#x0000000041;"), "AA") << "zero padding of any length";
    EXPECT_EQ(DecodeEntities("&#99999999999999999999;x"), "\xEF\xBF\xBDx");
}

TEST(HtmlLinks, SurvivesMalformedMarkup)
{
    const std::string_view inputs[] = {
        "<", "<a", "<a href", "<a href=", "<a href=\"unterminated", "<a href='x.html' <b", "<<<>>>", "<a =x href=y>",
        "<!--", "<script>", "<img src=\"a.png\" alt=", "<meta http-equiv=refresh content=\"0\">",
        "<meta http-equiv=refresh content=\"0;url=\">", "<a href=\"x\"\"y\">", "</a></a><a/href=z.html>",
    };
    for (const std::string_view input : inputs) {
        const PageLinks links = ScanLinks(input, 100);
        for (const auto& link : links.links) {
            EXPECT_FALSE(link.address.empty()) << input;
            EXPECT_LE(link.end, input.size()) << input;
            EXPECT_LE(link.begin, link.end) << input;
        }
    }
    EXPECT_EQ(Addresses(ScanLinks("<a href='x.html' <b>t</b>", 10)), std::vector<std::string>{"x.html"});
    EXPECT_EQ(Addresses(ScanLinks("</a></a><a/href=z.html>", 10)), std::vector<std::string>{"z.html"});
}

TEST(HtmlLinks, ReadsRefreshForms)
{
    EXPECT_EQ(Addresses(ScanLinks(R"(<meta http-equiv="refresh" content="0;url=a.html">)", 10)),
              std::vector<std::string>{"a.html"});
    EXPECT_EQ(Addresses(ScanLinks(R"(<meta http-equiv="refresh" content="3; b.html">)", 10)),
              std::vector<std::string>{"b.html"});
    EXPECT_EQ(Addresses(ScanLinks(R"(<meta http-equiv="refresh" content='1, URL = "c.html"'>)", 10)),
              std::vector<std::string>{"c.html"});
    EXPECT_TRUE(ScanLinks(R"(<meta http-equiv="refresh" content="30">)", 10).links.empty());
    EXPECT_TRUE(ScanLinks(R"(<meta name="refresh" content="0;url=d.html">)", 10).links.empty());
}

TEST(HtmlLinks, StopsAtTheCap)
{
    std::string html;
    for (int i = 0; i < 100'000; ++i) {
        html += "<a href=\"p" + std::to_string(i) + ".html\">" + std::to_string(i) + "</a>";
    }
    const PageLinks links = ScanLinks(html, 5000);
    EXPECT_EQ(links.links.size(), 5000u);
    EXPECT_TRUE(links.truncated);
    EXPECT_EQ(links.links.back().address, "p4999.html");
}

TEST(HtmlLinks, PositionsPointAtTheAddressAsWritten)
{
    const std::string html = R"(<a href=" a&amp;b.html ">A</a><img srcset="x.jpg 1x,y.jpg 2x"><a href=bare.html>B</a>)"
                             R"(<meta http-equiv="refresh" content="0; url=r.html">)";
    const PageLinks links = ScanLinks(html, 100);
    ASSERT_EQ(links.links.size(), 5u);
    EXPECT_EQ(html.substr(links.links[0].begin, links.links[0].end - links.links[0].begin), "a&amp;b.html");
    EXPECT_EQ(html.substr(links.links[1].begin, links.links[1].end - links.links[1].begin), "x.jpg");
    EXPECT_EQ(html.substr(links.links[2].begin, links.links[2].end - links.links[2].begin), "y.jpg");
    EXPECT_EQ(html.substr(links.links[3].begin, links.links[3].end - links.links[3].begin), "bare.html");
    EXPECT_EQ(html.substr(links.links[4].begin, links.links[4].end - links.links[4].begin), "r.html");
}

TEST(HtmlLinks, RewritesLinksByPosition)
{
    const std::string html = R"(<a href="http://example.com/a.html">A</a><img src=b.png srcset="b.png 1x, c.png 2x">)";
    const PageLinks links = ScanLinks(html, 100);
    ASSERT_EQ(links.links.size(), 4u);
    std::vector<LinkReplacement> replacements{
        {links.links[3].begin, links.links[3].end, "img/c.png"},
        {links.links[0].begin, links.links[0].end, "a.html"},
        {links.links[1].begin, links.links[1].end, "img/b.png"},
        {links.links[0].begin, links.links[0].end, "overlaps.html"},  // ignored
        {html.size() + 1, html.size() + 2, "outside"},                 // ignored
    };
    EXPECT_EQ(RewriteLinks(html, replacements),
              R"(<a href="a.html">A</a><img src=img/b.png srcset="b.png 1x, img/c.png 2x">)");
    EXPECT_EQ(RewriteLinks(html, {}), html);
}

}  // namespace
