#include "core/SiteTools.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace ldm::core {

namespace {

using Json = nlohmann::json;

struct ToolFiles {
    std::string_view releases;  // "https://github.com/<owner>/<repo>/releases/download/"
    std::string_view archive;
    std::string_view checksum;  // empty: the archive's name with ".sha256sum"
};

ToolFiles FilesOf(SiteTool tool)
{
#if defined(_M_ARM64)
    constexpr bool arm = true;
#else
    constexpr bool arm = false;
#endif
    if (tool == SiteTool::YtDlp) {
        return {"https://github.com/yt-dlp/yt-dlp/releases/download/", arm ? "yt-dlp_win_arm64.zip" : "yt-dlp_win.zip",
                "SHA2-256SUMS"};
    }
    return {"https://github.com/denoland/deno/releases/download/",
            arm ? "deno-aarch64-pc-windows-msvc.zip" : "deno-x86_64-pc-windows-msvc.zip", ""};
}

// A tag safe to put into an address: "2026.10.21", "v2.9.8".
bool IsSafeTag(std::string_view tag)
{
    return !tag.empty() && tag.size() <= 32 && tag.front() != '.' && std::all_of(tag.begin(), tag.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' ||
                      c == '_';
           });
}

bool IsHex64(std::string_view text)
{
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

std::string Lower(std::string_view text)
{
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return lower;
}

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '*')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace

std::string_view SiteToolName(SiteTool tool) noexcept
{
    return tool == SiteTool::YtDlp ? "yt-dlp" : "Deno";
}

std::string_view SiteToolFeedUrl(SiteTool tool) noexcept
{
    return tool == SiteTool::YtDlp ? "https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest"
                                   : "https://api.github.com/repos/denoland/deno/releases/latest";
}

std::optional<ToolRelease> ParseToolRelease(SiteTool tool, std::string_view json)
{
    const Json root = Json::parse(json.begin(), json.end(), nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return std::nullopt;
    }
    const auto tag = root.find("tag_name");
    const auto assets = root.find("assets");
    if (tag == root.end() || !tag->is_string() || assets == root.end() || !assets->is_array()) {
        return std::nullopt;
    }
    const std::string tagName = tag->get<std::string>();
    if (!IsSafeTag(tagName)) {
        return std::nullopt;
    }
    if (const auto draft = root.find("draft"); draft != root.end() && draft->is_boolean() && draft->get<bool>()) {
        return std::nullopt;
    }
    const ToolFiles files = FilesOf(tool);
    const std::string checksumName = files.checksum.empty() ? std::string(files.archive) + ".sha256sum" : std::string(files.checksum);
    std::optional<std::int64_t> archiveBytes;
    bool checksumListed = false;
    for (const Json& asset : *assets) {
        if (!asset.is_object()) {
            continue;
        }
        const auto name = asset.find("name");
        if (name == asset.end() || !name->is_string()) {
            continue;
        }
        if (name->get<std::string>() == files.archive) {
            const auto size = asset.find("size");
            if (size != asset.end() && size->is_number_integer()) {
                archiveBytes = size->get<std::int64_t>();
            }
        } else if (name->get<std::string>() == checksumName) {
            checksumListed = true;
        }
    }
    if (!archiveBytes || *archiveBytes <= 0 || *archiveBytes > kMaxToolArchiveBytes || !checksumListed) {
        return std::nullopt;
    }
    ToolRelease release;
    release.version = tagName.starts_with('v') ? tagName.substr(1) : tagName;
    release.archiveName = std::string(files.archive);
    const std::string folder = std::string(files.releases) + tagName + "/";
    release.archiveUrl = folder + release.archiveName;
    release.checksumUrl = folder + checksumName;
    release.archiveBytes = *archiveBytes;
    return release;
}

std::optional<std::string> ParseToolChecksum(SiteTool tool, std::string_view text, std::string_view archiveName)
{
    size_t start = 0;
    while (start < text.size()) {
        const size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view line = Trim(text.substr(start, end - start));
        start = end + 1;
        if (tool == SiteTool::Deno) {
            // "Hash      : CBA84B26..."
            if (line.starts_with("Hash")) {
                const size_t colon = line.find(':');
                if (colon != std::string_view::npos) {
                    const std::string_view value = Trim(line.substr(colon + 1));
                    if (IsHex64(value)) {
                        return Lower(value);
                    }
                }
            }
            continue;
        }
        // "<hash>  <name>", or "<hash> *<name>" for binary mode.
        if (line.size() < 66 || !IsHex64(line.substr(0, 64))) {
            continue;
        }
        if (Trim(line.substr(64)) == archiveName) {
            return Lower(line.substr(0, 64));
        }
    }
    return std::nullopt;
}

}  // namespace ldm::core
