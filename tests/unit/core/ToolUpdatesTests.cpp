#include "core/ToolUpdates.h"
#include "core/UpdateCheck.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using ldm::core::FetchFinished;
using ldm::core::FetchRequest;
using ldm::core::SiteTool;
using ldm::core::SiteToolsView;
using ldm::core::ToolInstalled;
using ldm::core::ToolUpdates;

std::string ReadFixture(const char* name)
{
    std::ifstream in(std::filesystem::path(LDM_SITE_TOOLS_FIXTURES) / name, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
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

class Proxy final : public ldm::core::ProxyResolver {
public:
    void Configure(const ldm::model::Settings&) override {}
    ldm::core::ProxyLookup Resolve(ldm::model::DownloadId id, std::int64_t, const std::string&) override
    {
        asked.push_back(id);
        ldm::core::ProxyLookup lookup;
        lookup.pending = pending;
        return lookup;
    }
    bool pending = false;
    std::vector<ldm::model::DownloadId> asked;
};

class FakeInstaller final : public ldm::core::ToolInstaller {
public:
    void Install(ldm::core::ToolInstallRequest request) override { installs.push_back(std::move(request)); }
    void Remove() override { ++removes; }
    std::optional<std::string> InstalledVersion(SiteTool tool) const override
    {
        ++reads;
        const auto it = versions.find(tool);
        return it == versions.end() ? std::nullopt : it->second;
    }
    std::vector<ldm::core::ToolInstallRequest> installs;
    int removes = 0;
    mutable int reads = 0;
    std::map<SiteTool, std::optional<std::string>> versions;
};

class ToolUpdatesTest : public ::testing::Test {
protected:
    ToolUpdatesTest() { settings_.siteVideosEnabled = true; }

    void Tick() { updates_.Tick(now_, nowUnixMs_, settings_); }
    // Answers the last fetch with `body`; returns what OnFetched returned.
    std::optional<SiteTool> Reply(std::string body)
    {
        FetchFinished finished;
        finished.fetchId = engine_.fetches.back().fetchId;
        finished.body = std::move(body);
        return updates_.OnFetched(finished, now_);
    }
    void Installed(SiteTool tool, std::string version, std::optional<std::string> failure = std::nullopt)
    {
        ToolInstalled installed;
        installed.tool = tool;
        installed.version = std::move(version);
        installed.failure = std::move(failure);
        updates_.OnInstalled(installed, now_);
    }

    RecordingEngine engine_;
    Proxy proxy_;
    FakeInstaller installer_;
    ToolUpdates updates_{engine_, proxy_, installer_};
    ldm::model::Settings settings_;
    std::chrono::steady_clock::time_point now_ = std::chrono::steady_clock::time_point{} + 10h;
    std::int64_t nowUnixMs_ = 1'792'909'519'000;
};

TEST_F(ToolUpdatesTest, NothingHappensWhileSiteVideosAreOff)
{
    settings_.siteVideosEnabled = false;
    Tick();
    EXPECT_TRUE(engine_.fetches.empty());
    EXPECT_EQ(updates_.NextDeadline(now_, nowUnixMs_, settings_), std::chrono::steady_clock::time_point::max());
}

TEST_F(ToolUpdatesTest, AMissingToolIsDownloadedCheckedAndHandedToTheInstaller)
{
    Tick();
    ASSERT_EQ(engine_.fetches.size(), 1u);
    const FetchRequest& release = engine_.fetches[0];
    EXPECT_EQ(release.url, "https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest");
    EXPECT_FALSE(release.userAgent.empty()) << "GitHub's API refuses requests without one";
    EXPECT_TRUE(ldm::core::IsToolFetchId(release.fetchId));
    EXPECT_EQ(updates_.View().state, SiteToolsView::State::Checking);

    EXPECT_EQ(Reply(ReadFixture("yt-dlp-release.json")), SiteTool::YtDlp) << "the check got an answer";
    settings_.siteVideosYtDlpCheckUnixMs = nowUnixMs_;  // as the coordinator stores it
    ASSERT_EQ(engine_.fetches.size(), 2u);
    EXPECT_EQ(engine_.fetches[1].url, "https://github.com/yt-dlp/yt-dlp/releases/download/2026.10.21/SHA2-256SUMS");

    EXPECT_FALSE(Reply(ReadFixture("yt-dlp-SHA2-256SUMS")));
    ASSERT_EQ(engine_.fetches.size(), 3u);
    const FetchRequest& archive = engine_.fetches[2];
    EXPECT_EQ(archive.url, "https://github.com/yt-dlp/yt-dlp/releases/download/2026.10.21/yt-dlp_win.zip");
    EXPECT_EQ(archive.maxBytes, 18'127'019u);
    ASSERT_TRUE(archive.totalTimeout);
    EXPECT_GE(*archive.totalTimeout, 10min);
    EXPECT_EQ(updates_.View().state, SiteToolsView::State::Downloading);
    EXPECT_EQ(updates_.View().archiveBytes, 18'127'019);

    EXPECT_FALSE(Reply(std::string(18'127'019, 'z')));
    ASSERT_EQ(installer_.installs.size(), 1u);
    EXPECT_EQ(installer_.installs[0].version, "2026.10.21");
    EXPECT_EQ(installer_.installs[0].sha256, "a981b50fff6e437576d1c3c8df0680af572028ca12dff5e49435d9d33c98294b");
    EXPECT_EQ(updates_.View().state, SiteToolsView::State::Installing);

    Installed(SiteTool::YtDlp, "2026.10.21");
    EXPECT_EQ(updates_.View().state, SiteToolsView::State::Idle);
    EXPECT_EQ(updates_.View().ytDlpVersion, "2026.10.21");
    Tick();
    EXPECT_EQ(engine_.fetches.back().url, "https://api.github.com/repos/denoland/deno/releases/latest") << "Deno next";
}

TEST_F(ToolUpdatesTest, ACurrentToolIsNotDownloadedAgain)
{
    installer_.versions[SiteTool::YtDlp] = "2026.10.21";
    installer_.versions[SiteTool::Deno] = "2.9.8";
    settings_.siteVideosYtDlpCheckUnixMs = nowUnixMs_ - 25 * 3'600'000;  // due
    settings_.siteVideosDenoCheckUnixMs = nowUnixMs_;
    Tick();
    ASSERT_EQ(engine_.fetches.size(), 1u);
    EXPECT_EQ(Reply(ReadFixture("yt-dlp-release.json")), SiteTool::YtDlp);
    EXPECT_EQ(engine_.fetches.size(), 1u);
    EXPECT_EQ(updates_.View().state, SiteToolsView::State::Idle);
}

TEST_F(ToolUpdatesTest, ChecksFollowTheirIntervalsAndTheDeadlineSaysWhen)
{
    installer_.versions[SiteTool::YtDlp] = "2026.10.21";
    installer_.versions[SiteTool::Deno] = "2.9.8";
    settings_.siteVideosYtDlpCheckUnixMs = nowUnixMs_ - 23 * 3'600'000;  // due in an hour
    settings_.siteVideosDenoCheckUnixMs = nowUnixMs_ - 6 * 24 * 3'600'000;  // due in a day
    Tick();
    EXPECT_TRUE(engine_.fetches.empty());
    EXPECT_EQ(updates_.NextDeadline(now_, nowUnixMs_, settings_), now_ + 1h);
    // A clock set back by a day makes both look checked in the future: asked again at once.
    nowUnixMs_ -= 2 * 24 * 3'600'000;
    Tick();
    EXPECT_EQ(engine_.fetches.size(), 1u);
}

TEST_F(ToolUpdatesTest, AFailedAttemptIsRetriedAfterAnHourAndShowsItsReason)
{
    Tick();
    FetchFinished failed;
    failed.fetchId = engine_.fetches.back().fetchId;
    failed.failure = ldm::core::TransferFailure{ldm::core::FailureReason::Dns, ldm::core::FailureClass::Retryable, 0,
                                                "could not resolve host"};
    EXPECT_FALSE(updates_.OnFetched(failed, now_));
    EXPECT_NE(updates_.View().lastError.find("could not resolve host"), std::string::npos);

    Tick();  // yt-dlp waits now, so Deno, also missing, goes first
    EXPECT_EQ(engine_.fetches.back().url, "https://api.github.com/repos/denoland/deno/releases/latest");
    EXPECT_FALSE(Reply("garbage"));
    const size_t asked = engine_.fetches.size();
    now_ += 59min;
    Tick();
    EXPECT_EQ(engine_.fetches.size(), asked);
    now_ += 2min;
    Tick();
    EXPECT_EQ(engine_.fetches.size(), asked + 1);
}

TEST_F(ToolUpdatesTest, AnIncompleteArchiveOrAFailedInstallIsNotKept)
{
    Tick();
    (void)Reply(ReadFixture("yt-dlp-release.json"));
    (void)Reply(ReadFixture("yt-dlp-SHA2-256SUMS"));
    EXPECT_FALSE(Reply("short"));
    EXPECT_TRUE(installer_.installs.empty());
    EXPECT_NE(updates_.View().lastError.find("incomplete"), std::string::npos);

    updates_.CheckNow();
    Tick();
    (void)Reply(ReadFixture("yt-dlp-release.json"));
    (void)Reply(ReadFixture("yt-dlp-SHA2-256SUMS"));
    (void)Reply(std::string(18'127'019, 'z'));
    Installed(SiteTool::YtDlp, "2026.10.21", std::string("The download of yt-dlp does not match its published checksum."));
    EXPECT_FALSE(updates_.View().ytDlpVersion);
    EXPECT_EQ(updates_.View().lastError, "The download of yt-dlp does not match its published checksum.");
}

TEST_F(ToolUpdatesTest, LateAnswersAfterTurningOffOrRemovingAreIgnored)
{
    Tick();
    const std::int64_t first = engine_.fetches.back().fetchId;
    settings_.siteVideosEnabled = false;
    Tick();
    FetchFinished late;
    late.fetchId = first;
    late.body = ReadFixture("yt-dlp-release.json");
    EXPECT_FALSE(updates_.OnFetched(late, now_));
    EXPECT_EQ(engine_.fetches.size(), 1u);

    settings_.siteVideosEnabled = true;
    Tick();
    updates_.Remove();
    EXPECT_EQ(installer_.removes, 1);
    EXPECT_EQ(updates_.View().state, SiteToolsView::State::Removing);
    Tick();
    EXPECT_EQ(engine_.fetches.size(), 2u) << "nothing starts while removing";
    ToolInstalled removed;
    removed.action = ToolInstalled::Action::Remove;
    updates_.OnInstalled(removed, now_);
    EXPECT_EQ(updates_.View().state, SiteToolsView::State::Idle);
}

TEST_F(ToolUpdatesTest, LookupTroubleChecksYtDlpAtMostHourly)
{
    installer_.versions[SiteTool::YtDlp] = "2026.10.21";
    installer_.versions[SiteTool::Deno] = "2.9.8";
    settings_.siteVideosYtDlpCheckUnixMs = nowUnixMs_;
    settings_.siteVideosDenoCheckUnixMs = nowUnixMs_;
    updates_.ResolverTrouble(now_);
    Tick();
    ASSERT_EQ(engine_.fetches.size(), 1u);
    (void)Reply(ReadFixture("yt-dlp-release.json"));
    updates_.ResolverTrouble(now_ + 30min);
    Tick();
    EXPECT_EQ(engine_.fetches.size(), 1u);
    updates_.ResolverTrouble(now_ + 61min);
    Tick();
    EXPECT_EQ(engine_.fetches.size(), 2u);
}

TEST_F(ToolUpdatesTest, AFetchWaitsForItsProxy)
{
    proxy_.pending = true;
    Tick();
    EXPECT_TRUE(engine_.fetches.empty());
    ASSERT_EQ(proxy_.asked.size(), 1u);
    EXPECT_TRUE(ldm::core::IsToolFetchId(proxy_.asked[0].value));
    updates_.OnProxyResolved(proxy_.asked[0].value, std::string("http://proxy.test:3128"));
    ASSERT_EQ(engine_.fetches.size(), 1u);
    EXPECT_EQ(engine_.fetches[0].proxy, "http://proxy.test:3128");
}

TEST_F(ToolUpdatesTest, InstalledVersionsAreReadFromDiskOnce)
{
    installer_.versions[SiteTool::YtDlp] = "2026.10.21";
    (void)updates_.View();
    (void)updates_.View();
    Tick();
    EXPECT_EQ(installer_.reads, 2) << "one per tool";
}

TEST_F(ToolUpdatesTest, ToolIdsDoNotCollideWithOtherFetches)
{
    EXPECT_FALSE(ldm::core::IsToolFetchId(ldm::core::kUpdateFetchId));
    EXPECT_FALSE(ldm::core::IsToolFetchId(-1));
    EXPECT_FALSE(ldm::core::IsToolFetchId(ldm::core::kToolEventId));
    EXPECT_FALSE(ldm::core::IsToolFetchId(-(std::int64_t{1} << 39)));
}

}  // namespace
