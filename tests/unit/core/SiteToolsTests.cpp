#include "core/SiteTools.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

using ldm::core::ParseToolChecksum;
using ldm::core::ParseToolRelease;
using ldm::core::SiteTool;

// GitHub's answers for yt-dlp 2026.10.21 and Deno v2.9.8, kept to tag, draft flag and assets, and
// the checksum files of those releases (tests/fixtures/site-tools).
std::string ReadFixture(const char* name)
{
    std::ifstream in(std::filesystem::path(LDM_SITE_TOOLS_FIXTURES) / name, std::ios::binary);
    EXPECT_TRUE(in) << name;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

TEST(SiteTools, YtDlpReleaseGivesTheFolderBuildAndItsChecksumFile)
{
    const auto release = ParseToolRelease(SiteTool::YtDlp, ReadFixture("yt-dlp-release.json"));
    ASSERT_TRUE(release);
    EXPECT_EQ(release->version, "2026.10.21");
    EXPECT_EQ(release->archiveName, "yt-dlp_win.zip");
    EXPECT_EQ(release->archiveUrl, "https://github.com/yt-dlp/yt-dlp/releases/download/2026.10.21/yt-dlp_win.zip");
    EXPECT_EQ(release->checksumUrl, "https://github.com/yt-dlp/yt-dlp/releases/download/2026.10.21/SHA2-256SUMS");
    EXPECT_EQ(release->archiveBytes, 18'127'019);
}

TEST(SiteTools, DenoReleaseDropsTheTagsLeadingV)
{
    const auto release = ParseToolRelease(SiteTool::Deno, ReadFixture("deno-release.json"));
    ASSERT_TRUE(release);
    EXPECT_EQ(release->version, "2.9.8");
    EXPECT_EQ(release->archiveUrl, "https://github.com/denoland/deno/releases/download/v2.9.8/deno-x86_64-pc-windows-msvc.zip");
    EXPECT_EQ(release->checksumUrl,
              "https://github.com/denoland/deno/releases/download/v2.9.8/deno-x86_64-pc-windows-msvc.zip.sha256sum");
    EXPECT_EQ(release->archiveBytes, 42'917'472);
}

TEST(SiteTools, TheAddressesNeverComeFromTheAnswer)
{
    const std::string json = R"({"tag_name":"2026.10.21","assets":[
        {"name":"yt-dlp_win.zip","size":100,"browser_download_url":"https://evil.example/yt-dlp_win.zip"},
        {"name":"SHA2-256SUMS","size":10,"browser_download_url":"https://evil.example/SHA2-256SUMS"}]})";
    const auto release = ParseToolRelease(SiteTool::YtDlp, json);
    ASSERT_TRUE(release);
    EXPECT_EQ(release->archiveUrl.rfind("https://github.com/yt-dlp/yt-dlp/releases/download/", 0), 0u);
}

TEST(SiteTools, UnusableReleasesAreRefused)
{
    for (const char* json : {
             "",
             "[]",
             R"({"assets":[]})",
             R"({"tag_name":"../../x","assets":[{"name":"yt-dlp_win.zip","size":1},{"name":"SHA2-256SUMS","size":1}]})",
             R"({"tag_name":"a/b","assets":[{"name":"yt-dlp_win.zip","size":1},{"name":"SHA2-256SUMS","size":1}]})",
             R"({"tag_name":"1","draft":true,"assets":[{"name":"yt-dlp_win.zip","size":1},{"name":"SHA2-256SUMS","size":1}]})",
             R"({"tag_name":"1","assets":[{"name":"yt-dlp_win.zip","size":1}]})",
             R"({"tag_name":"1","assets":[{"name":"SHA2-256SUMS","size":1}]})",
             R"({"tag_name":"1","assets":[{"name":"yt-dlp_win.zip","size":0},{"name":"SHA2-256SUMS","size":1}]})",
             R"({"tag_name":"1","assets":[{"name":"yt-dlp_win.zip","size":200000000},{"name":"SHA2-256SUMS","size":1}]})",
         }) {
        EXPECT_FALSE(ParseToolRelease(SiteTool::YtDlp, json)) << json;
    }
}

TEST(SiteTools, ChecksumsAreReadFromEachProjectsOwnFormat)
{
    EXPECT_EQ(ParseToolChecksum(SiteTool::YtDlp, ReadFixture("yt-dlp-SHA2-256SUMS"), "yt-dlp_win.zip"),
              "a981b50fff6e437576d1c3c8df0680af572028ca12dff5e49435d9d33c98294b");
    EXPECT_EQ(ParseToolChecksum(SiteTool::Deno, ReadFixture("deno.sha256sum"), "deno-x86_64-pc-windows-msvc.zip"),
              "cba84b26e0df5ae7f74fc66b86a95b866872a86292ebd84fa1e959927edd93bf");
    // Another file's line, or none at all.
    EXPECT_FALSE(ParseToolChecksum(SiteTool::YtDlp, ReadFixture("yt-dlp-SHA2-256SUMS"), "yt-dlp_win.zip.exe"));
    EXPECT_FALSE(ParseToolChecksum(SiteTool::YtDlp, "not a checksum file", "yt-dlp_win.zip"));
    EXPECT_FALSE(ParseToolChecksum(SiteTool::Deno, "Hash : 1234", "deno-x86_64-pc-windows-msvc.zip"));
}

}  // namespace
