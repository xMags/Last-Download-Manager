#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ldm::media {

// A parsed XML element, as much of XML as the DASH parser needs. Built by a platform XML reader
// (platform/win/XmlLiteReader) so the parser itself stays free of Win32 and is tested directly.
struct XmlElement {
    std::string name;  // the local name: "MPD" for <MPD> and for <mpd:MPD>
    // Attributes without a namespace prefix, as DASH defines its own. Prefixed ones (xlink:href,
    // cenc:default_KID) and namespace declarations are left out, so they cannot be mistaken for them.
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<XmlElement> children;
    // Character data directly inside this element, concatenated. Runs of only whitespace (such as
    // indentation, or spaces after a CDATA section) are left out, so callers trim what they read.
    std::string text;

    [[nodiscard]] const std::string* Attribute(std::string_view attributeName) const;
    [[nodiscard]] const XmlElement* Child(std::string_view childName) const;  // the first one
    [[nodiscard]] std::vector<const XmlElement*> Children(std::string_view childName) const;
};

}  // namespace ldm::media
