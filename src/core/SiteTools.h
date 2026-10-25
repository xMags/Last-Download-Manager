#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The programs LDM downloads for site videos (video-sites design, section 6): yt-dlp, which says
// what a video page offers, and Deno, the JavaScript runtime yt-dlp needs for some sites. Both come
// from their projects' GitHub releases, are checked against the checksums published there, and
// live in LDM's tools folder. This file reads the releases' answers; nothing here touches disk.
namespace ldm::core {

enum class SiteTool {
    YtDlp,
    Deno,
};

[[nodiscard]] std::string_view SiteToolName(SiteTool tool) noexcept;  // "yt-dlp", "Deno"

// GitHub's "latest release" answer for the tool.
[[nodiscard]] std::string_view SiteToolFeedUrl(SiteTool tool) noexcept;

// What to download for one release, for the processor LDM was built for.
struct ToolRelease {
    std::string version;      // "2026.10.21", "2.9.8"
    std::string archiveName;  // "yt-dlp_win.zip"
    std::string archiveUrl;   // built by LDM from the release and the name, never taken from the answer
    std::string checksumUrl;
    std::int64_t archiveBytes = 0;  // as the release states it
};

// The largest archive LDM downloads; Deno's is about 43 MB.
inline constexpr std::int64_t kMaxToolArchiveBytes = 96 * 1024 * 1024;

// Reads a release answer (untrusted). Nothing when it does not name a release with the archive
// and checksum file LDM needs, or the archive is larger than kMaxToolArchiveBytes.
[[nodiscard]] std::optional<ToolRelease> ParseToolRelease(SiteTool tool, std::string_view json);

// The SHA-256 of `archiveName`, in lower-case hex, from the release's checksum file: yt-dlp's
// SHA2-256SUMS lists "<hash>  <name>" lines, Deno's .sha256sum holds PowerShell's Get-FileHash
// output. Nothing when the file does not state one.
[[nodiscard]] std::optional<std::string> ParseToolChecksum(SiteTool tool, std::string_view text,
                                                           std::string_view archiveName);

}  // namespace ldm::core
