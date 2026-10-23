#pragma once

#include "media/MediaModel.h"

#include <string>
#include <string_view>

namespace ldm::media {

// The name the DRM notice shows for a protection identifier: an HLS KEYFORMAT or a DASH
// ContentProtection@schemeIdUri. Unknown identifiers are returned as they are.
[[nodiscard]] std::string DrmSystemName(std::string_view identifier);

// Adds `system` to `drm` (creating it) unless it is listed already.
void AddDrmSystem(std::optional<DrmInfo>& drm, std::string system);

}  // namespace ldm::media
