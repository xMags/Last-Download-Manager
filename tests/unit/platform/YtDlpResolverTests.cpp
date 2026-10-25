#include "platform/win/YtDlpResolver.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using ldm::platform::ReadResolverFailure;
using ldm::platform::YtDlpArguments;
using ldm::platform::YtDlpTools;
using Problem = ldm::core::SiteLookupFinished::Problem;

YtDlpTools Tools()
{
    YtDlpTools tools;
    tools.ytDlp = LR"(C:\Users\u\AppData\Local\Last Download Manager\tools\yt-dlp\yt-dlp.exe)";
    tools.deno = LR"(C:\Users\u\AppData\Local\Last Download Manager\tools\deno\deno.exe)";
    tools.cacheFolder = LR"(C:\Users\u\AppData\Local\Last Download Manager\tools\cache)";
    tools.ffmpegFolder = LR"(C:\Program Files\LDM)";
    return tools;
}

// The value after `option`, or "(missing)".
std::wstring After(const std::vector<std::wstring>& arguments, const std::wstring& option)
{
    const auto it = std::find(arguments.begin(), arguments.end(), option);
    return it == arguments.end() || it + 1 == arguments.end() ? L"(missing)" : *(it + 1);
}

TEST(YtDlpResolver, ArgumentsFixEveryBehaviourAndEndWithThePageAfterTheOptions)
{
    ldm::core::SiteLookupRequest request{3, "https://www.youtube.com/watch?v=-oops--flag", std::string("http://p.test:8080")};
    const auto arguments = YtDlpArguments(Tools(), request);
    for (const wchar_t* flag : {L"--ignore-config", L"--no-playlist", L"--flat-playlist", L"--no-warnings",
                                L"--no-progress", L"--dump-single-json"}) {
        EXPECT_NE(std::find(arguments.begin(), arguments.end(), flag), arguments.end()) << flag;
    }
    EXPECT_EQ(After(arguments, L"--proxy"), L"http://p.test:8080");
    EXPECT_EQ(After(arguments, L"--js-runtimes"), LR"(deno:C:\Users\u\AppData\Local\Last Download Manager\tools\deno\deno.exe)");
    EXPECT_EQ(After(arguments, L"--ffmpeg-location"), LR"(C:\Program Files\LDM)");
    EXPECT_EQ(After(arguments, L"--cache-dir"), LR"(C:\Users\u\AppData\Local\Last Download Manager\tools\cache)");
    ASSERT_GE(arguments.size(), 2u);
    EXPECT_EQ(arguments[arguments.size() - 2], L"--");
    EXPECT_EQ(arguments.back(), L"https://www.youtube.com/watch?v=-oops--flag");
}

TEST(YtDlpResolver, NoProxyMeansAnEmptyOneSoNoSystemProxyIsUsedBehindLdmsBack)
{
    const auto arguments = YtDlpArguments(Tools(), ldm::core::SiteLookupRequest{1, "https://www.youtube.com/watch?v=jNQXAC9IVRw", {}});
    EXPECT_EQ(After(arguments, L"--proxy"), L"");
}

TEST(YtDlpResolver, FactsAboutTheVideoAreTheVideosProblemWithYtDlpsPrefixRemoved)
{
    const auto gone = ReadResolverFailure("WARNING: x\nERROR: [youtube] xxxxxxxxxxx: This video is unavailable\n");
    EXPECT_EQ(gone.problem, Problem::Video);
    EXPECT_EQ(gone.message, "This video is unavailable");
    const auto age = ReadResolverFailure(
        "ERROR: [youtube] abcdefghijk: Sign in to confirm your age. This video may be inappropriate for some users.\r\n");
    EXPECT_EQ(age.problem, Problem::Video);
    EXPECT_EQ(age.message, "Sign in to confirm your age. This video may be inappropriate for some users");
    EXPECT_EQ(ReadResolverFailure("ERROR: [youtube] abcdefghijk: Private video. Sign in if you've been granted access")
                  .problem,
              Problem::Video);
}

TEST(YtDlpResolver, ABotCheckIsShownInPlainWords)
{
    const auto bot = ReadResolverFailure("ERROR: [youtube] abcdefghijk: Sign in to confirm you\xE2\x80\x99re not a bot.");
    EXPECT_EQ(bot.problem, Problem::Video);
    EXPECT_EQ(bot.message, "The site asked to confirm that this is not a bot. Try again later.");
}

TEST(YtDlpResolver, OtherErrorsAreTheResolversAndLoseYtDlpsBugReportAdvice)
{
    const auto broken = ReadResolverFailure(
        "ERROR: [youtube] abcdefghijk: Unable to extract player response; please report this issue on "
        "https://github.com/yt-dlp/yt-dlp/issues\n");
    EXPECT_EQ(broken.problem, Problem::Resolver);
    EXPECT_EQ(broken.message, "Unable to extract player response");
    const auto nothing = ReadResolverFailure("Traceback (most recent call last):\n  ...\n");
    EXPECT_EQ(nothing.problem, Problem::Resolver);
    EXPECT_EQ(nothing.message, "The video site's page could not be read.");
    const auto plain = ReadResolverFailure("ERROR: Unsupported URL: https://example.test/\n");
    EXPECT_EQ(plain.message, "Unsupported URL: https://example.test/");
}

TEST(YtDlpResolver, AVeryLongErrorIsCut)
{
    const auto long_ = ReadResolverFailure("ERROR: " + std::string(1000, 'x'));
    EXPECT_LE(long_.message.size(), 300u);
}

TEST(YtDlpResolver, ASignInIsHandedOverAsACookiesFileBeforeThePage)
{
    const ldm::core::SiteLookupRequest request{1, "https://vimeo.com/76979871", {}, "vimeo.com\tFALSE\t/\tTRUE\t0\tvuid\tv\n"};
    const std::filesystem::path file = LR"(C:\Users\u\AppData\Local\Last Download Manager\tools\cache\sign-in-1.txt)";
    const auto arguments = YtDlpArguments(Tools(), request, file);
    EXPECT_EQ(After(arguments, L"--cookies"), file.native());
    EXPECT_EQ(arguments.back(), L"https://vimeo.com/76979871");
    EXPECT_EQ(After(YtDlpArguments(Tools(), request), L"--cookies"), L"(missing)") << "only with a file";
}

}  // namespace
