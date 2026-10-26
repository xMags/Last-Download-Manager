// YtDlpResolver against a stand-in for yt-dlp (FakeSiteResolver.cpp): what it runs, how it reads
// answers and failures, and stopping lookups that are queued or running.
#include "core/SiteResolver.h"
#include "platform/win/YtDlpResolver.h"
#include "RecordingListener.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using ldm::core::SiteLookupFinished;
using ldm::core::SiteLookupKey;
using ldm::core::SiteLookupRequest;
using Problem = SiteLookupFinished::Problem;

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

class SiteResolverTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        root_ = std::filesystem::temp_directory_path() / (L"ldm-site-resolver-" + std::to_wstring(::GetCurrentProcessId()));
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_ / L"answers");
        std::filesystem::create_directories(root_ / L"yt-dlp");
        std::filesystem::create_directories(root_ / L"deno");
        std::filesystem::copy_file(LDM_FAKE_SITE_RESOLVER_EXE, root_ / L"yt-dlp" / L"yt-dlp.exe");
        WriteFile(root_ / L"deno" / L"deno.exe", "");  // only its presence is checked
        ::SetEnvironmentVariableW(L"LDM_FAKE_SITE_DIR", (root_ / L"answers").c_str());
        tools_.ytDlp = root_ / L"yt-dlp" / L"yt-dlp.exe";
        tools_.deno = root_ / L"deno" / L"deno.exe";
        tools_.cacheFolder = root_ / L"cache";
        tools_.ffmpegFolder = root_;
    }

    void TearDown() override
    {
        ::SetEnvironmentVariableW(L"LDM_FAKE_SITE_DIR", nullptr);
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    // What the stand-in does for video `id`.
    void Prepare(const std::string& id, const std::string& json, const std::string& errors = "", int code = 0, int sleepMs = 0)
    {
        const auto base = root_ / L"answers" / std::filesystem::path(id);
        WriteFile(base.wstring() + L".json", json);
        WriteFile(base.wstring() + L".err", errors);
        WriteFile(base.wstring() + L".code", std::to_string(code));
        if (sleepMs > 0) {
            WriteFile(base.wstring() + L".sleep", std::to_string(sleepMs));
        }
    }

    static SiteLookupRequest Request(std::int64_t lookupId, const std::string& id)
    {
        return SiteLookupRequest{lookupId, "https://www.youtube.com/watch?v=" + id, std::nullopt};
    }

    std::optional<SiteLookupFinished> Answer(std::int64_t lookupId, std::chrono::milliseconds timeout = 30s)
    {
        return listener_.WaitFor<SiteLookupFinished>(SiteLookupKey(lookupId), 0, timeout);
    }

    std::filesystem::path root_;
    ldm::platform::YtDlpTools tools_;
    ldm::test::RecordingListener listener_;
};

TEST_F(SiteResolverTest, AnAnswerIsReadWhileWarningsOnStandardErrorStayOutOfIt)
{
    Prepare("jNQXAC9IVRw", ReadFile(std::filesystem::path(LDM_MEDIA_FIXTURES) / "site" / "youtube-short.json"),
            "WARNING: [youtube] something the site changed\n");
    ldm::platform::YtDlpResolver resolver(tools_, listener_);
    resolver.Resolve(Request(1, "jNQXAC9IVRw"));
    const auto answer = Answer(1);
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->lookupId, 1);
    ASSERT_TRUE(answer->video) << answer->failure;
    EXPECT_EQ(answer->video->id, "jNQXAC9IVRw");
    EXPECT_EQ(answer->video->offer.variants.size(), 2u);

    // The stand-in got LDM's options and the page last.
    std::string args = ReadFile(root_ / L"answers" / L"jNQXAC9IVRw.args");
    std::erase(args, '\r');  // written in text mode
    EXPECT_NE(args.find("--ignore-config\n"), std::string::npos) << args;
    EXPECT_NE(args.find("--\nhttps://www.youtube.com/watch?v=jNQXAC9IVRw\n"), std::string::npos) << args;
    EXPECT_TRUE(std::filesystem::is_directory(tools_.cacheFolder)) << "made for yt-dlp";
}

TEST_F(SiteResolverTest, AFailureGivesYtDlpsReasonAndWhoseProblemItIs)
{
    Prepare("AAAAAAAAAAA", "null\n", "ERROR: [youtube] AAAAAAAAAAA: This video is unavailable\n", 1);
    Prepare("BBBBBBBBBBB", "", "ERROR: [youtube] BBBBBBBBBBB: Unable to extract player response; please report this issue\n", 1);
    Prepare("CCCCCCCCCCC", R"({"_type":"playlist","id":"PL"})");
    Prepare("DDDDDDDDDDD", "this is not JSON");
    ldm::platform::YtDlpResolver resolver(tools_, listener_);
    resolver.Resolve(Request(1, "AAAAAAAAAAA"));
    resolver.Resolve(Request(2, "BBBBBBBBBBB"));
    resolver.Resolve(Request(3, "CCCCCCCCCCC"));
    resolver.Resolve(Request(4, "DDDDDDDDDDD"));

    const auto gone = Answer(1);
    ASSERT_TRUE(gone);
    EXPECT_EQ(gone->problem, Problem::Video);
    EXPECT_EQ(gone->failure, "This video is unavailable");
    const auto broken = Answer(2);
    ASSERT_TRUE(broken);
    EXPECT_EQ(broken->problem, Problem::Resolver);
    EXPECT_EQ(broken->failure, "Unable to extract player response");
    const auto playlist = Answer(3);
    ASSERT_TRUE(playlist);
    EXPECT_EQ(playlist->problem, Problem::Video);
    const auto garbage = Answer(4);
    ASSERT_TRUE(garbage);
    EXPECT_EQ(garbage->problem, Problem::Resolver);
    EXPECT_FALSE(garbage->video);
}

TEST_F(SiteResolverTest, SignInFilesLeftByAnEarlierRunAreRemovedAtStart)
{
    std::filesystem::create_directories(tools_.cacheFolder);
    WriteFile(tools_.cacheFolder / L"sign-in-3.txt", "# Netscape HTTP Cookie File");
    WriteFile(tools_.cacheFolder / L"other.txt", "yt-dlp's own");
    ldm::platform::YtDlpResolver resolver(tools_, listener_);
    EXPECT_FALSE(std::filesystem::exists(tools_.cacheFolder / L"sign-in-3.txt")) << "it held cookies in plain text";
    EXPECT_TRUE(std::filesystem::exists(tools_.cacheFolder / L"other.txt"));
}

TEST_F(SiteResolverTest, MissingToolsAreSaidSoWithoutRunningAnything)
{
    std::filesystem::remove(tools_.deno);
    ldm::platform::YtDlpResolver resolver(tools_, listener_);
    resolver.Resolve(Request(1, "jNQXAC9IVRw"));
    const auto answer = Answer(1);
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->problem, Problem::ToolsMissing);
    EXPECT_FALSE(std::filesystem::exists(root_ / L"answers" / L"jNQXAC9IVRw.args"));
}

TEST_F(SiteResolverTest, CancellingARunningLookupEndsItAndAQueuedOneIsAnsweredAtOnce)
{
    for (const char* id : {"SLOW1aaaaaa", "SLOW2aaaaaa", "QUEUEDaaaaa"}) {
        Prepare(id, "{}", "", 0, 20'000);
    }
    ldm::platform::YtDlpResolver resolver(tools_, listener_);
    resolver.Resolve(Request(1, "SLOW1aaaaaa"));
    resolver.Resolve(Request(2, "SLOW2aaaaaa"));
    resolver.Resolve(Request(3, "QUEUEDaaaaa"));  // both workers are busy
    std::this_thread::sleep_for(500ms);
    const auto started = std::chrono::steady_clock::now();
    resolver.Cancel(3);
    resolver.Cancel(1);
    const auto queued = Answer(3, 5s);
    ASSERT_TRUE(queued);
    EXPECT_TRUE(queued->stopped);
    const auto running = Answer(1, 10s);
    ASSERT_TRUE(running);
    EXPECT_TRUE(running->stopped);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 10s);
    EXPECT_FALSE(std::filesystem::exists(root_ / L"answers" / L"QUEUEDaaaaa.args")) << "never started";
}

TEST_F(SiteResolverTest, DestroyingTheResolverEndsRunningLookupsQuickly)
{
    Prepare("SLOW1aaaaaa", "{}", "", 0, 20'000);
    const auto started = std::chrono::steady_clock::now();
    {
        ldm::platform::YtDlpResolver resolver(tools_, listener_);
        resolver.Resolve(Request(1, "SLOW1aaaaaa"));
        std::this_thread::sleep_for(500ms);
    }
    EXPECT_LT(std::chrono::steady_clock::now() - started, 10s);
}

}  // namespace
