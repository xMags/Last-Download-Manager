#pragma once

#include "browser/Protocol.h"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

// Native messaging host registration data (M3 design section 3). Writing the files and registry
// keys is platform work; this part only builds the content, so it can be tested anywhere.
namespace ldm::browser {

// Lower-case dotted name the browsers use to find the host in the registry.
inline constexpr std::string_view kHostName = "io.github.mrdevfx.ldm";
// The unpacked Chromium extension's id, fixed by the public "key" in manifest.chromium.json: the
// first 32 hex digits of the SHA-256 of that key, written with the letters a to p.
inline constexpr std::string_view kChromiumExtensionId = "jdkneeflbkgkkinfcpfamlckpoknbnln";
// Every Chromium extension id allowed to start the host: the unpacked build's, and the ids the
// Chrome Web Store and Edge Add-ons assign once the extension is published there (their packages
// leave out the "key", so the stores choose their own).
inline constexpr std::array<std::string_view, 1> kChromiumExtensionIds{kChromiumExtensionId};
// The Firefox add-on id, fixed by browser_specific_settings.gecko.id in manifest.firefox.json.
inline constexpr std::string_view kFirefoxExtensionId = "ldm-integration@mrdevfx.github.io";

// The manifest JSON for one browser family, pointing at ldm-host.exe.
[[nodiscard]] std::string NativeHostManifest(BrowserFamily family, const std::filesystem::path& hostExecutable);

}  // namespace ldm::browser
