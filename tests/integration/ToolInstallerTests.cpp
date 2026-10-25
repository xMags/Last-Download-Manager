// WinToolInstaller with real zip archives (made with Windows' tar.exe) holding the yt-dlp stand-in:
// checksum, unpacking, the program's own check, replacing an installed version, waiting for a
// running lookup, and removing everything.
#include "platform/win/ChildProcess.h"
#include "platform/win/SiteToolsFolder.h"
#include "platform/win/WinToolInstaller.h"
#include "RecordingListener.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <shared_mutex>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using ldm::core::SiteTool;
using ldm::core::ToolInstalled;
using ldm::core::ToolInstallRequest;

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteFile(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::filesystem::path Tar()
{
    std::array<wchar_t, MAX_PATH> system{};
    const UINT length = ::GetSystemDirectoryW(system.data(), MAX_PATH);
    return std::filesystem::path(std::wstring(system.data(), length)) / L"tar.exe";
}

std::filesystem::path WorkFolder()
{
    return std::filesystem::temp_directory_path() / (L"ldm-tool-installer-" + std::to_wstring(::GetCurrentProcessId()));
}

class ToolInstallerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        std::filesystem::remove_all(work_);
        std::filesystem::create_directories(work_ / L"content" / L"_internal");
        std::filesystem::copy_file(LDM_FAKE_SITE_RESOLVER_EXE, work_ / L"content" / L"yt-dlp.exe");
        WriteFile(work_ / L"content" / L"_internal" / L"library.txt", "part of the folder build");
        ldm::platform::ChildProcessRequest pack;
        pack.program = Tar();
        pack.arguments = {L"-a", L"-c", L"-f", (work_ / L"yt-dlp_win.zip").native(), L"-C", (work_ / L"content").native(),
                          L"yt-dlp.exe", L"_internal"};
        const auto packed = ldm::platform::RunChildProcess(pack);
        ASSERT_TRUE(packed && packed.Value().exitCode == 0) << (packed ? packed.Value().output : packed.GetError().message);
        archive_ = ReadFile(work_ / L"yt-dlp_win.zip");
        ::SetEnvironmentVariableW(L"LDM_FAKE_VERSION", L"2026.10.21");
    }

    void TearDown() override
    {
        ::SetEnvironmentVariableW(L"LDM_FAKE_VERSION", nullptr);
        std::error_code ignored;
        std::filesystem::remove_all(work_, ignored);
    }

    ToolInstallRequest Request(std::string version = "2026.10.21") const
    {
        return ToolInstallRequest{SiteTool::YtDlp, std::move(version), archive_, ldm::platform::Sha256Hex(archive_)};
    }

    // The next answer after those already taken.
    std::optional<ToolInstalled> Next(std::chrono::milliseconds timeout = 60s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto all = listener_.All<ToolInstalled>(ldm::model::DownloadId{ldm::core::kToolEventId}, 0);
            if (all.size() > taken_) {
                return all[taken_++];
            }
            std::this_thread::sleep_for(20ms);
        }
        return std::nullopt;
    }

    std::filesystem::path Tools() const { return work_ / L"tools"; }

    const std::filesystem::path work_ = WorkFolder();  // before folder_, which is built from it
    std::string archive_;
    ldm::test::RecordingListener listener_;
    ldm::platform::SiteToolsFolder folder_{work_ / L"tools", work_};
    size_t taken_ = 0;
};

TEST_F(ToolInstallerTest, Sha256MatchesTheStandardTestVectors)
{
    EXPECT_EQ(ldm::platform::Sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(ldm::platform::Sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_F(ToolInstallerTest, AnArchiveIsCheckedUnpackedTriedAndPutInPlace)
{
    ldm::platform::WinToolInstaller installer(folder_, listener_);
    EXPECT_FALSE(installer.InstalledVersion(SiteTool::YtDlp));
    installer.Install(Request());
    const auto done = Next();
    ASSERT_TRUE(done);
    EXPECT_FALSE(done->failure) << done->failure.value_or("");
    EXPECT_EQ(done->version, "2026.10.21");
    EXPECT_EQ(installer.InstalledVersion(SiteTool::YtDlp), "2026.10.21");
    EXPECT_TRUE(std::filesystem::is_regular_file(Tools() / L"yt-dlp" / L"_internal" / L"library.txt"));
    EXPECT_FALSE(std::filesystem::exists(Tools() / L"yt-dlp.new"));
    EXPECT_FALSE(std::filesystem::exists(Tools() / L"yt-dlp.old"));
    EXPECT_FALSE(std::filesystem::exists(Tools() / L"yt-dlp.download.zip"));
}

TEST_F(ToolInstallerTest, AWrongChecksumInstallsNothing)
{
    ldm::platform::WinToolInstaller installer(folder_, listener_);
    ToolInstallRequest request = Request();
    request.sha256 = std::string(64, '0');
    installer.Install(request);
    const auto done = Next();
    ASSERT_TRUE(done);
    ASSERT_TRUE(done->failure);
    EXPECT_NE(done->failure->find("checksum"), std::string::npos);
    EXPECT_FALSE(installer.InstalledVersion(SiteTool::YtDlp));
    EXPECT_FALSE(std::filesystem::exists(Tools() / L"yt-dlp"));
}

TEST_F(ToolInstallerTest, ANewVersionReplacesTheOldOneButOneThatFailsItsCheckDoesNot)
{
    ldm::platform::WinToolInstaller installer(folder_, listener_);
    installer.Install(Request());
    ASSERT_TRUE(Next());

    ::SetEnvironmentVariableW(L"LDM_FAKE_VERSION", L"2026.11.02");
    installer.Install(Request("2026.11.02"));
    const auto upgraded = Next();
    ASSERT_TRUE(upgraded);
    EXPECT_FALSE(upgraded->failure) << upgraded->failure.value_or("");
    EXPECT_EQ(installer.InstalledVersion(SiteTool::YtDlp), "2026.11.02");

    // The program answers with another version than the release says: it is not put in place.
    installer.Install(Request("2026.12.01"));
    const auto refused = Next();
    ASSERT_TRUE(refused);
    ASSERT_TRUE(refused->failure);
    EXPECT_NE(refused->failure->find("kept"), std::string::npos) << *refused->failure;
    EXPECT_EQ(installer.InstalledVersion(SiteTool::YtDlp), "2026.11.02");
    EXPECT_FALSE(std::filesystem::exists(Tools() / L"yt-dlp.new"));
}

TEST_F(ToolInstallerTest, ReplacingWaitsForARunningLookup)
{
    ldm::platform::WinToolInstaller installer(folder_, listener_);
    std::shared_lock running(folder_.Lock());  // as YtDlpResolver holds it while yt-dlp runs
    installer.Install(Request());
    EXPECT_FALSE(Next(3s)) << "unpacked and checked, but not put in place while the lookup runs";
    EXPECT_FALSE(std::filesystem::exists(Tools() / L"yt-dlp"));
    running.unlock();
    const auto done = Next();
    ASSERT_TRUE(done);
    EXPECT_FALSE(done->failure);
}

TEST_F(ToolInstallerTest, RemoveDeletesTheToolsAndTheCache)
{
    ldm::platform::WinToolInstaller installer(folder_, listener_);
    installer.Install(Request());
    ASSERT_TRUE(Next());
    std::filesystem::create_directories(Tools() / L"cache" / L"youtube-nsig");
    installer.Remove();
    const auto removed = Next();
    ASSERT_TRUE(removed);
    EXPECT_EQ(removed->action, ToolInstalled::Action::Remove);
    EXPECT_FALSE(removed->failure) << removed->failure.value_or("");
    EXPECT_FALSE(std::filesystem::exists(Tools()));
    EXPECT_FALSE(installer.InstalledVersion(SiteTool::YtDlp));
}

TEST_F(ToolInstallerTest, AVersionFileWithoutItsProgramMeansNotInstalled)
{
    ldm::platform::WinToolInstaller installer(folder_, listener_);
    std::filesystem::create_directories(Tools() / L"deno");
    WriteFile(Tools() / L"deno" / L"ldm-version.txt", "2.9.8\n");
    EXPECT_FALSE(installer.InstalledVersion(SiteTool::Deno));
    WriteFile(Tools() / L"deno" / L"deno.exe", "");
    EXPECT_EQ(installer.InstalledVersion(SiteTool::Deno), "2.9.8");
    WriteFile(Tools() / L"deno" / L"ldm-version.txt", "../../evil\n");
    EXPECT_FALSE(installer.InstalledVersion(SiteTool::Deno));
}

}  // namespace
