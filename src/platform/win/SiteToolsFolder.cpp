#include "platform/win/SiteToolsFolder.h"

#include <utility>

namespace ldm::platform {

SiteToolsFolder::SiteToolsFolder(std::filesystem::path root, std::filesystem::path ffmpegFolder)
    : root_(std::move(root)), ffmpegFolder_(std::move(ffmpegFolder))
{
}

std::filesystem::path SiteToolsFolder::FolderOf(core::SiteTool tool) const
{
    return root_ / (tool == core::SiteTool::YtDlp ? L"yt-dlp" : L"deno");
}

std::filesystem::path SiteToolsFolder::ProgramOf(core::SiteTool tool) const
{
    return FolderOf(tool) / (tool == core::SiteTool::YtDlp ? L"yt-dlp.exe" : L"deno.exe");
}

std::filesystem::path SiteToolsFolder::VersionFileOf(core::SiteTool tool) const
{
    return FolderOf(tool) / L"ldm-version.txt";
}

YtDlpTools SiteToolsFolder::ResolverTools() const
{
    YtDlpTools tools;
    tools.ytDlp = ProgramOf(core::SiteTool::YtDlp);
    tools.deno = ProgramOf(core::SiteTool::Deno);
    tools.cacheFolder = root_ / L"cache";
    tools.ffmpegFolder = ffmpegFolder_;
    return tools;
}

}  // namespace ldm::platform
