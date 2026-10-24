#include "core/UpdateCheck.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace {

using ldm::core::FetchFinished;
using ldm::core::FetchRequest;
using ldm::core::IsNewerVersion;
using ldm::core::ParseLatestRelease;
using ldm::core::UpdateCheck;
using std::chrono_literals::operator""s;
using std::chrono_literals::operator""min;
using std::chrono_literals::operator""h;

// A trimmed answer of GitHub's GET /repos/{owner}/{repo}/releases/latest.
constexpr char kGitHubAnswer[] = R"({
  "url": "https://api.github.com/repos/xMags/Last-Download-Manager/releases/1",
  "html_url": "https://github.com/xMags/Last-Download-Manager/releases/tag/v5.1.0",
  "id": 1, "tag_name": "v5.1.0", "name": "5.1.0", "draft": false, "prerelease": false,
  "assets": [{"name": "LastDownloadManager-windows-x64-setup.exe", "size": 17908284}],
  "body": "Notes"
})";

TEST(UpdateCheck, ReadsGitHubsLatestRelease)
{
    const auto release = ParseLatestRelease(kGitHubAnswer);
    ASSERT_TRUE(release);
    EXPECT_EQ(release->version, "5.1.0");
    EXPECT_EQ(release->pageUrl, "https://github.com/xMags/Last-Download-Manager/releases/tag/v5.1.0");
    EXPECT_FALSE(release->prerelease);
}

TEST(UpdateCheck, RejectsAnswersThatAreNotAReleaseOfGitHub)
{
    const std::string bad[] = {
        "",
        "not json",
        "[]",
        R"({"html_url":"https://github.com/x/y/releases/tag/v5.1.0"})",
        R"({"tag_name":"v5.1.0"})",
        R"({"tag_name":5,"html_url":"https://github.com/x"})",
        R"({"tag_name":"latest","html_url":"https://github.com/x"})",
        R"({"tag_name":"v5.1","html_url":"https://github.com/x"})",
        R"({"tag_name":"v5.1.0","html_url":"http://github.com/x"})",
        R"({"tag_name":"v5.1.0","html_url":"https://evil.example/github.com"})",
        R"({"tag_name":"v5.1.0","html_url":"https://github.com.evil.example/x"})",
        R"({"tag_name":"v5.1.0","html_url":"https://user@github.com/x"})",
        R"({"tag_name":"v5.1.0","html_url":"javascript:alert"})",
        R"({"tag_name":"v5.1.0","html_url":"https://github.com/x","draft":true})",
    };
    for (const std::string& json : bad) {
        EXPECT_FALSE(ParseLatestRelease(json)) << json;
    }
    EXPECT_FALSE(ParseLatestRelease(std::string(2 * 1024 * 1024, ' ')));
}

TEST(UpdateCheck, ComparesVersions)
{
    struct Case {
        const char* candidate;
        const char* current;
        bool newer;
    };
    const Case cases[] = {
        {"5.0.1", "5.0.0", true},     {"5.1.0", "5.0.9", true},          {"6.0.0", "5.99.99", true},
        {"5.0.0", "5.0.0", false},    {"4.9.9", "5.0.0", false},         {"v5.0.1", "5.0.0", true},
        {"5.0.10", "5.0.9", true},    {"5.1.0-beta.1", "5.0.0", false},  {"5.1.0-beta.2", "5.1.0-beta.1", true},
        {"5.1.0", "5.1.0-beta.3", true}, {"5.1.0-beta.10", "5.1.0-beta.9", true}, {"5.1.0-beta.1", "5.1.0-alpha.1", true},
        {"5.1.0-alpha.1", "5.1.0-alpha", true}, {"5.0.0+build.7", "5.0.0", false}, {"5.0", "4.0.0", false},
        {"05.0.0", "4.0.0", false},   {"5.0.0-", "4.0.0", false},        {"x.y.z", "1.0.0", false},
        {"5.0.1", "garbage", false},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(IsNewerVersion(c.candidate, c.current), c.newer) << c.candidate << " vs " << c.current;
    }
}

class RecordingEngine final : public ldm::core::TransferEngine {
public:
    void Start(ldm::core::TransferRequest) override {}
    void StartMedia(ldm::core::MediaRequest) override {}
    void ExtendMedia(ldm::model::DownloadId, std::int64_t, ldm::core::MediaExtension) override {}
    void Stop(ldm::model::DownloadId, std::int64_t) override {}
    void SetSpeedLimit(ldm::model::DownloadId, std::int64_t, std::optional<std::int64_t>) override {}
    void SetGlobalSpeedLimit(std::optional<std::int64_t>) override {}
    void Finalize(ldm::core::FinalizeRequest) override {}
    void Fetch(FetchRequest request) override { fetches.push_back(std::move(request)); }
    std::vector<FetchRequest> fetches;
};

class DirectProxy final : public ldm::core::ProxyResolver {
public:
    void Configure(const ldm::model::Settings&) override {}
    ldm::core::ProxyLookup Resolve(ldm::model::DownloadId, std::int64_t, const std::string&) override { return {}; }
};

FetchFinished Answer(std::string body)
{
    FetchFinished finished;
    finished.fetchId = ldm::core::kUpdateFetchId;
    finished.body = std::move(body);
    return finished;
}

TEST(UpdateCheck, ChecksOnlyWhenTurnedOnAndDue)
{
    RecordingEngine engine;
    DirectProxy proxy;
    const auto start = std::chrono::steady_clock::time_point{} + 1h;
    UpdateCheck check(engine, proxy, "https://feed.test/latest", "5.0.0", start);
    ldm::model::Settings settings;
    const std::int64_t day = 24 * 3600 * 1000LL;
    const std::int64_t now = 1'800'000'000'000;

    check.Tick(start + 120s, now, settings);
    EXPECT_TRUE(engine.fetches.empty()) << "off until the user turns it on";

    settings.updatesCheckEnabled = true;
    check.Tick(start + 30s, now, settings);
    EXPECT_TRUE(engine.fetches.empty()) << "not in the first minute";
    settings.updatesLastCheckUnixMs = now - 3 * day;
    check.Tick(start + 120s, now, settings);
    EXPECT_TRUE(engine.fetches.empty()) << "checked three days ago";

    settings.updatesLastCheckUnixMs = now - 15 * day;
    check.Tick(start + 2h, now, settings);
    ASSERT_EQ(engine.fetches.size(), 1u);
    EXPECT_EQ(engine.fetches[0].url, "https://feed.test/latest");
    EXPECT_EQ(engine.fetches[0].fetchId, ldm::core::kUpdateFetchId);
    EXPECT_EQ(engine.fetches[0].userAgent, "LastDownloadManager/5.0.0");
    EXPECT_LE(engine.fetches[0].maxBytes, 1024u * 1024u);
    check.Tick(start + 4h, now, settings);
    EXPECT_EQ(engine.fetches.size(), 1u) << "one check at a time";

    FetchFinished wrong = Answer(kGitHubAnswer);
    wrong.fetchId = 5;
    EXPECT_FALSE(check.OnFetched(wrong, settings)) << "not its answer";
    const auto outcome = check.OnFetched(Answer(kGitHubAnswer), settings);
    ASSERT_TRUE(outcome);
    EXPECT_FALSE(outcome->manual);
    EXPECT_FALSE(outcome->error);
    ASSERT_TRUE(outcome->newer);
    EXPECT_EQ(outcome->newer->version, "5.1.0");

    // The coordinator records the answered check; the next one is two weeks later.
    settings.updatesLastCheckUnixMs = now;
    check.Tick(start + 6h, now + 6 * 3600 * 1000LL, settings);
    EXPECT_EQ(engine.fetches.size(), 1u);
    check.Tick(start + 15 * 24h, now + 15 * day, settings);
    EXPECT_EQ(engine.fetches.size(), 2u);
}

TEST(UpdateCheck, AFailedCheckIsTriedAgainADayLater)
{
    RecordingEngine engine;
    DirectProxy proxy;
    const auto start = std::chrono::steady_clock::time_point{} + 1h;
    UpdateCheck check(engine, proxy, "https://feed.test/latest", "5.0.0", start);
    ldm::model::Settings settings;
    settings.updatesCheckEnabled = true;
    const std::int64_t now = 1'800'000'000'000;
    check.Tick(start + 2min, now, settings);
    ASSERT_EQ(engine.fetches.size(), 1u) << "never checked: due a minute after the start";
    FetchFinished failed = Answer("");
    failed.failure = ldm::core::TransferFailure{ldm::core::FailureReason::Dns, ldm::core::FailureClass::Retryable, 0, "no dns"};
    const auto failure = check.OnFetched(failed, settings);
    ASSERT_TRUE(failure && failure->error);
    EXPECT_FALSE(failure->newer);
    check.Tick(start + 3h, now, settings);
    EXPECT_EQ(engine.fetches.size(), 1u);
    check.Tick(start + 26h, now, settings);
    EXPECT_EQ(engine.fetches.size(), 2u);
    const auto garbage = check.OnFetched(Answer("<html>rate limited</html>"), settings);
    ASSERT_TRUE(garbage && garbage->error);
}

TEST(UpdateCheck, ASkippedVersionIsOfferedOnlyWhenAsked)
{
    RecordingEngine engine;
    DirectProxy proxy;
    const auto start = std::chrono::steady_clock::time_point{} + 1h;
    UpdateCheck check(engine, proxy, "https://feed.test/latest", "5.0.0", start);
    ldm::model::Settings settings;
    settings.updatesCheckEnabled = true;
    settings.updatesSkippedVersion = "5.1.0";
    check.Tick(start + 2min, 1'800'000'000'000, settings);
    ASSERT_EQ(engine.fetches.size(), 1u);
    auto outcome = check.OnFetched(Answer(kGitHubAnswer), settings);
    ASSERT_TRUE(outcome);
    EXPECT_FALSE(outcome->newer);

    check.CheckNow();
    ASSERT_EQ(engine.fetches.size(), 2u) << "asking works whatever the settings say";
    outcome = check.OnFetched(Answer(kGitHubAnswer), settings);
    ASSERT_TRUE(outcome);
    EXPECT_TRUE(outcome->manual);
    ASSERT_TRUE(outcome->newer);

    // The running version is the newest.
    check.CheckNow();
    outcome = check.OnFetched(Answer(R"({"tag_name":"v5.0.0","html_url":"https://github.com/x"})"), settings);
    ASSERT_TRUE(outcome);
    EXPECT_TRUE(outcome->manual);
    EXPECT_FALSE(outcome->newer);
    EXPECT_FALSE(outcome->error);
}

}  // namespace
