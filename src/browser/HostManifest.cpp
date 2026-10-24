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
        nlohmann::json origins = nlohmann::json::array();
        for (const std::string_view id : kChromiumExtensionIds) {
            origins.push_back("chrome-extension://" + std::string(id) + "/");
        }
        manifest["allowed_origins"] = std::move(origins);
    }
    return manifest.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace ldm::browser
