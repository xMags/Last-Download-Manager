#pragma once

#include "core/SiteTools.h"
#include "core/TransferEngine.h"
#include "model/Ids.h"

#include <cstdint>
#include <optional>
#include <string>

// The port to LDM's tools folder (video-sites design, section 6): checks a downloaded archive
// against its published SHA-256, unpacks it, tries the program, and only then puts it in place of
// the old one. Implemented in platform/win on a worker thread.
namespace ldm::core {

struct ToolInstallRequest {
    SiteTool tool = SiteTool::YtDlp;
    std::string version;
    std::string archive;  // the zip's bytes
    std::string sha256;   // lower-case hex, from the release's checksum file
};

// Answers come as ToolInstalled events with this id.
inline constexpr std::int64_t kToolEventId = -(std::int64_t{1} << 38) - 2048;

// Every method is thread-safe. Install and Remove return at once and are answered by exactly one
// ToolInstalled event each, in the order asked.
class ToolInstaller {
public:
    virtual ~ToolInstaller() = default;
    virtual void Install(ToolInstallRequest request) = 0;
    // Deletes every tool and the resolver's cache.
    virtual void Remove() = 0;
    // The installed version, read from the tools folder; nothing when the tool is not there.
    [[nodiscard]] virtual std::optional<std::string> InstalledVersion(SiteTool tool) const = 0;
};

}  // namespace ldm::core
