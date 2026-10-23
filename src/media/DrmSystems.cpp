#include "media/DrmSystems.h"

#include <algorithm>
#include <array>
#include <utility>

namespace ldm::media {

namespace {

// System IDs from the DASH-IF registry, and the HLS key formats Apple and Microsoft document.
constexpr std::array<std::pair<std::string_view, std::string_view>, 7> kKnownSystems{{
    {"urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed", "Widevine"},
    {"urn:uuid:9a04f079-9840-4286-ab92-e65be0885f95", "PlayReady"},
    {"urn:uuid:94ce86fb-07ff-4f43-adb8-93d2fa968ca2", "FairPlay"},
    {"urn:uuid:1077efec-c0b2-4d02-ace3-3c1e52e2fb4b", "ClearKey"},
    {"urn:mpeg:dash:mp4protection:2011", "Common Encryption"},
    {"com.apple.streamingkeydelivery", "FairPlay"},
    {"com.microsoft.playready", "PlayReady"},
}};

std::string LowerAscii(std::string_view text)
{
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; });
    return lower;
}

}  // namespace

std::string DrmSystemName(std::string_view identifier)
{
    const std::string lower = LowerAscii(identifier);
    for (const auto& [known, name] : kKnownSystems) {
        if (lower == known) {
            return std::string(name);
        }
    }
    return std::string(identifier);
}

void AddDrmSystem(std::optional<DrmInfo>& drm, std::string system)
{
    if (!drm) {
        drm.emplace();
    }
    if (std::find(drm->systems.begin(), drm->systems.end(), system) == drm->systems.end()) {
        drm->systems.push_back(std::move(system));
    }
}

}  // namespace ldm::media
