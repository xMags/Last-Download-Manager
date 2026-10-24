#pragma once

#include "base/Result.h"
#include "media/XmlElement.h"

#include <cstddef>
#include <string_view>

// Reads XML documents from web sites (DASH manifests, M3 design section 6) with XmlLite, which is
// part of Windows. The input is hostile: DTDs are refused, which rules out entity expansion and
// external entities, and the size, nesting depth and element count are capped.
namespace ldm::platform {

inline constexpr std::size_t kMaxXmlDepth = 64;
inline constexpr std::size_t kMaxXmlElements = 500'000;

// The document's root element. `bytes` is the document as served: UTF-8 or UTF-16 with a byte
// order mark, or whatever its XML declaration names. At most media::kMaxManifestBytes.
[[nodiscard]] base::Result<media::XmlElement> ReadXml(std::string_view bytes);

}  // namespace ldm::platform
