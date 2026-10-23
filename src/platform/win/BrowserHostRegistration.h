#pragma once

#include <filesystem>

namespace ldm::platform {

// Registers ldm-host.exe as the native messaging host for Chrome, Edge and Firefox for the current
// user (M3 design section 3): writes one manifest per browser family into `manifestFolder` and
// points each browser's HKCU key at it. Needs no administrator rights. Runs on every start and
// rewrites only what differs, so a moved LDM folder is picked up. Returns false when anything
// failed; browsers whose registration succeeded still work.
bool RegisterBrowserHosts(const std::filesystem::path& manifestFolder, const std::filesystem::path& hostExecutable);

}  // namespace ldm::platform
