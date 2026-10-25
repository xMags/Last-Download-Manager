#pragma once

#include "core/SiteTools.h"
#include "platform/win/YtDlpResolver.h"

#include <filesystem>
#include <shared_mutex>

// LDM's tools folder for site videos (video-sites design, section 6), in the profile beside the
// database: tools\yt-dlp\ (the folder build), tools\deno\, and yt-dlp's cache in tools\cache\.
// Shared by the resolver, which runs the programs, and the installer, which replaces them: a run
// holds the lock shared; replacing or deleting a program's folder holds it exclusively, because
// Windows cannot rename a folder while a program in it runs.
namespace ldm::platform {

class SiteToolsFolder {
public:
    // `ffmpegFolder`: where the ffmpeg.exe LDM ships is, for yt-dlp.
    SiteToolsFolder(std::filesystem::path root, std::filesystem::path ffmpegFolder);
    SiteToolsFolder(const SiteToolsFolder&) = delete;
    SiteToolsFolder& operator=(const SiteToolsFolder&) = delete;

    [[nodiscard]] const std::filesystem::path& Root() const noexcept { return root_; }
    [[nodiscard]] std::filesystem::path FolderOf(core::SiteTool tool) const;
    [[nodiscard]] std::filesystem::path ProgramOf(core::SiteTool tool) const;
    // The file the installer writes the installed version into, inside the tool's folder.
    [[nodiscard]] std::filesystem::path VersionFileOf(core::SiteTool tool) const;
    [[nodiscard]] YtDlpTools ResolverTools() const;

    [[nodiscard]] std::shared_mutex& Lock() noexcept { return lock_; }

private:
    const std::filesystem::path root_;
    const std::filesystem::path ffmpegFolder_;
    std::shared_mutex lock_;
};

}  // namespace ldm::platform
