#include "media/MediaModel.h"
#include "platform/win/XmlLiteReader.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using ldm::platform::ReadXml;

TEST(XmlLiteReader, BuildsTheTree)
{
    const auto root = ReadXml(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<mpd:MPD xmlns:mpd=\"urn:mpeg:dash:schema:mpd:2011\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
        "type=\"static\">\n"
        "  <mpd:BaseURL>  media/<![CDATA[a&b]]>  </mpd:BaseURL>\n"
        "  <mpd:Period xlink:href=\"https://ads.example.test/p\" id=\"p&amp;1\"/>\n"
        "  <mpd:Period id=\"p2\"><!-- a comment --></mpd:Period>\n"
        "</mpd:MPD>");
    ASSERT_TRUE(root) << root.GetError().message;
    EXPECT_EQ(root.Value().name, "MPD") << "the local name, without the prefix";
    ASSERT_EQ(root.Value().attributes.size(), 1u) << "namespace declarations are left out";
    EXPECT_EQ(*root.Value().Attribute("type"), "static");
    ASSERT_NE(root.Value().Child("BaseURL"), nullptr);
    EXPECT_EQ(root.Value().Child("BaseURL")->text, "  media/a&b") << "a run of only whitespace is left out";
    const auto periods = root.Value().Children("Period");
    ASSERT_EQ(periods.size(), 2u);
    EXPECT_EQ(*periods[0]->Attribute("id"), "p&1");
    EXPECT_EQ(periods[0]->Attribute("href"), nullptr) << "prefixed attributes are left out";
    EXPECT_EQ(*periods[1]->Attribute("id"), "p2");
}

TEST(XmlLiteReader, ReadsUtf16WithAByteOrderMark)
{
    const std::u16string text = u"<MPD lang=\"\u00e9\"/>";
    std::string bytes = "\xFF\xFE";
    for (const char16_t c : text) {
        bytes.push_back(static_cast<char>(c & 0xFF));
        bytes.push_back(static_cast<char>(c >> 8));
    }
    const auto root = ReadXml(bytes);
    ASSERT_TRUE(root) << root.GetError().message;
    EXPECT_EQ(*root.Value().Attribute("lang"), "\xC3\xA9");
}

TEST(XmlLiteReader, RefusesDtdsAndEntities)
{
    const std::string billionLaughs =
        "<?xml version=\"1.0\"?><!DOCTYPE MPD [<!ENTITY a \"aaaaaaaaaa\">"
        "<!ENTITY b \"&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;\"><!ENTITY c \"&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;\">]>"
        "<MPD>&c;</MPD>";
    EXPECT_FALSE(ReadXml(billionLaughs));
    EXPECT_FALSE(ReadXml("<?xml version=\"1.0\"?><!DOCTYPE MPD [<!ENTITY x SYSTEM \"file:///C:/Windows/win.ini\">]>"
                         "<MPD>&x;</MPD>"));
    EXPECT_FALSE(ReadXml("<!DOCTYPE MPD><MPD/>"));
    EXPECT_FALSE(ReadXml("<MPD>&undefined;</MPD>"));
}

TEST(XmlLiteReader, RefusesMalformedAndOversizedDocuments)
{
    EXPECT_FALSE(ReadXml(""));
    EXPECT_FALSE(ReadXml("not xml"));
    EXPECT_FALSE(ReadXml("<MPD>"));
    EXPECT_FALSE(ReadXml("<MPD></Period>"));
    EXPECT_FALSE(ReadXml("<MPD a=\"1\" a=\"2\"/>"));

    std::string deep;
    for (size_t i = 0; i <= ldm::platform::kMaxXmlDepth; ++i) {
        deep += "<a>";
    }
    for (size_t i = 0; i <= ldm::platform::kMaxXmlDepth; ++i) {
        deep += "</a>";
    }
    EXPECT_FALSE(ReadXml(deep)) << "nesting deeper than allowed";

    std::string many = "<MPD>";
    many.reserve(ldm::platform::kMaxXmlElements * 4 + 16);
    for (size_t i = 0; i < ldm::platform::kMaxXmlElements; ++i) {
        many += "<a/>";
    }
    many += "</MPD>";
    EXPECT_FALSE(ReadXml(many)) << "one element more than allowed";

    EXPECT_FALSE(ReadXml(std::string(ldm::media::kMaxManifestBytes + 1, ' ')));
}

}  // namespace
