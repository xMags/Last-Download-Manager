#include "browser/HostManifest.h"

#include "base/Utf.h"

#include <nlohmann/json.hpp>

namespace ldm::browser {

std::string NativeHostManifest(BrowserFamily family, const std::filesystem::path& hostExecutable)
{
    nlohmann::json manifest = {
        {"name", std::string(kHostName)},
        {"description", "Last Download Manager browser integration"},
        {"path", base::ToUtf8(hostExecutable.wstring())},
        {"type", "stdio"},
    };
    if (family == BrowserFamily::Firefox) {
        manifest["allowed_extensions"] = {std::string(kFirefoxExtensionId)};
    } else {
        manifest["allowed_origins"] = {"chrome-extension://" + std::string(kChromiumExtensionId) + "/"};
    }
    return manifest.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace ldm::browser
