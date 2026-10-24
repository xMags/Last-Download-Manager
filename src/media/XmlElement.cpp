#include "media/XmlElement.h"

#include <algorithm>

namespace ldm::media {

const std::string* XmlElement::Attribute(std::string_view attributeName) const
{
    const auto found = std::find_if(attributes.begin(), attributes.end(),
                                    [attributeName](const auto& entry) { return entry.first == attributeName; });
    return found == attributes.end() ? nullptr : &found->second;
}

const XmlElement* XmlElement::Child(std::string_view childName) const
{
    const auto found = std::find_if(children.begin(), children.end(),
                                    [childName](const XmlElement& child) { return child.name == childName; });
    return found == children.end() ? nullptr : &*found;
}

std::vector<const XmlElement*> XmlElement::Children(std::string_view childName) const
{
    std::vector<const XmlElement*> found;
    for (const XmlElement& child : children) {
        if (child.name == childName) {
            found.push_back(&child);
        }
    }
    return found;
}

}  // namespace ldm::media
