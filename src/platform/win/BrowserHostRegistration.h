#pragma once

#include <filesystem>

namespace ldm::platform {

// Registers ldm-host.exe as the native messaging host for Chrome, Edge and Firefox for the current
// user (M3 design section 3): writes one manifest per browser family into `manifestFolder` and
// points each browser's HKCU key at it. Needs no administrator rights. Runs on every start and
// rewrites only what differs, so a moved LDM folder is picked up. Returns false when anything
// failed; browsers whose registration succeeded still work.
bool RegisterBrowserHosts(const std::filesystem::path& manifestFolder, const std::filesystem::path& hostExecutable);

// True when LDM runs elevated ("Run as administrator"). Browsers run without elevation, and
// Windows does not let their ldm-host.exe reach an elevated LDM, so capture stops working
// (checklist B-10).
[[nodiscard]] bool IsProcessElevated();

}  // namespace ldm::platform
