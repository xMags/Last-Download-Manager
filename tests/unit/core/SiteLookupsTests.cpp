#include "core/SiteLookups.h"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace {

using ldm::core::LookUpSiteVideoCommand;
using ldm::core::SiteLookupFinished;
using ldm::core::SiteLookupKey;
using ldm::core::SiteLookupRequest;
using ldm::core::SiteLookups;
using ldm::model::DownloadId;
using Problem = SiteLookupFinished::Problem;

constexpr const char* kPage = "https://www.youtube.com/watch?v=aqz-KE-bpKQ";

class FakeResolver final : public ldm::core::SiteResolver {
public:
    void Resolve(SiteLookupRequest request) override { asked.push_back(std::move(request)); }
    void Cancel(std::int64_t lookupId) override { cancelled.push_back(lookupId); }
    std::vector<SiteLookupRequest> asked;
    std::vector<std::int64_t> cancelled;
};

class FakeProxy final : public ldm::core::ProxyResolver {
public:
    void Configure(const ldm::model::Settings&) override {}
    ldm::core::ProxyLookup Resolve(DownloadId id, std::int64_t, const std::string& url) override
    {
        questions.emplace_back(id, url);
        ldm::core::ProxyLookup lookup;
        lookup.pending = pending;
        lookup.proxy = proxy;
        return lookup;
    }
    bool pending = false;
    std::optional<std::string> proxy;
    std::vector<std::pair<DownloadId, std::string>> questions;
};

class FakeObserver final : public ldm::core::CoreObserver {
public:
    void OnSnapshot(std::shared_ptr<const ldm::core::Snapshot>) override {}
    void OnNotification(ldm::core::Notification notification) override
    {
        if (auto* looked = std::get_if<SiteLookupFinished>(&notification)) {
            answers.push_back(*looked);
        }
    }
    std::vector<SiteLookupFinished> answers;
};

class FakeClock final : public ldm::core::Clock {
public:
    std::int64_t NowUnixMs() override { return 0; }
    std::chrono::steady_clock::time_point Steady() override { return now; }
    std::chrono::steady_clock::time_point now{std::chrono::hours(1)};
};

SiteLookupFinished Found(const char* id = "aqz-KE-bpKQ")
{
    SiteLookupFinished finished;
    ldm::media::SiteVideo video;
    video.id = id;
    video.title = "Big Buck Bunny";
    finished.video = video;
    return finished;
}

class SiteLookupsTest : public ::testing::Test {
protected:
    SiteLookupsTest()
        : lookups_(resolver_, proxy_, observer_, clock_, [this](DownloadId id, std::int64_t epoch, const SiteLookupFinished& f) {
              refreshed_.emplace_back(id, epoch, f);
              if (react_) {
                  react_(id, epoch);
              }
          })
    {
    }

    void LookUp(std::int64_t panelId, const char* page = kPage) { lookups_.LookUp(LookUpSiteVideoCommand{panelId, page}); }
    // The resolver answers its n-th question.
    void Answer(size_t question, SiteLookupFinished finished)
    {
        finished.lookupId = resolver_.asked.at(question).lookupId;
        lookups_.OnFinished(SiteLookupKey(finished.lookupId), finished);
    }

    FakeResolver resolver_;
    FakeProxy proxy_;
    FakeObserver observer_;
    FakeClock clock_;
    std::vector<std::tuple<DownloadId, std::int64_t, SiteLookupFinished>> refreshed_;
    std::function<void(DownloadId, std::int64_t)> react_;  // runs after each refresh answer
    SiteLookups lookups_;
};

TEST_F(SiteLookupsTest, TheResolverIsAskedAboutTheCanonicalPageAndThePanelGetsTheAnswerUnderItsId)
{
    proxy_.proxy = "http://proxy.test:3128";
    LookUp(7, "https://youtu.be/aqz-KE-bpKQ?t=5");
    ASSERT_EQ(resolver_.asked.size(), 1u);
    EXPECT_EQ(resolver_.asked[0].pageUrl, kPage);
    EXPECT_EQ(resolver_.asked[0].proxy, "http://proxy.test:3128");
    ASSERT_EQ(proxy_.questions.size(), 1u);
    EXPECT_TRUE(ldm::core::IsSiteLookupKey(proxy_.questions[0].first));
    EXPECT_FALSE(ldm::core::IsSiteLookupKey(DownloadId{-1})) << "a manifest fetch's key";
    EXPECT_FALSE(ldm::core::IsSiteLookupKey(DownloadId{-(std::int64_t{1} << 40)})) << "the Grabber's";
    EXPECT_FALSE(ldm::core::IsSiteLookupKey(DownloadId{-(std::int64_t{1} << 38)})) << "the update check's";
    EXPECT_TRUE(observer_.answers.empty());

    Answer(0, Found());
    ASSERT_EQ(observer_.answers.size(), 1u);
    EXPECT_EQ(observer_.answers[0].lookupId, 7);
    ASSERT_TRUE(observer_.answers[0].video);
    EXPECT_EQ(observer_.answers[0].video->title, "Big Buck Bunny");
}

TEST_F(SiteLookupsTest, TwoTabsOnTheSameVideoShareOneLookup)
{
    LookUp(1);
    LookUp(2, "https://www.youtube.com/watch?v=aqz-KE-bpKQ&list=PL1");
    ASSERT_EQ(resolver_.asked.size(), 1u);
    Answer(0, Found());
    ASSERT_EQ(observer_.answers.size(), 2u);
    EXPECT_EQ(observer_.answers[0].lookupId, 1);
    EXPECT_EQ(observer_.answers[1].lookupId, 2);
}

TEST_F(SiteLookupsTest, ARecentAnswerIsGivenAgainWithoutAskingUntilItIsTooOld)
{
    LookUp(1);
    Answer(0, Found());
    clock_.now += std::chrono::minutes(29);
    LookUp(2);
    EXPECT_EQ(resolver_.asked.size(), 1u);
    ASSERT_EQ(observer_.answers.size(), 2u);
    EXPECT_TRUE(observer_.answers[1].video);

    clock_.now += std::chrono::minutes(2);
    LookUp(3);
    EXPECT_EQ(resolver_.asked.size(), 2u) << "31 minutes old: asked again";
}

TEST_F(SiteLookupsTest, FailuresAreNotKept)
{
    LookUp(1);
    SiteLookupFinished failed;
    failed.problem = Problem::Video;
    failed.failure = "Private video";
    Answer(0, failed);
    ASSERT_EQ(observer_.answers.size(), 1u);
    EXPECT_EQ(observer_.answers[0].failure, "Private video");
    LookUp(2);
    EXPECT_EQ(resolver_.asked.size(), 2u);
}

TEST_F(SiteLookupsTest, CancellingTheOnlyWaiterStopsTheLookupAndItsLateAnswerIsDropped)
{
    LookUp(1);
    LookUp(2);
    lookups_.Cancel(1);
    EXPECT_TRUE(resolver_.cancelled.empty()) << "tab 2 still waits";
    lookups_.Cancel(2);
    ASSERT_EQ(resolver_.cancelled.size(), 1u);
    EXPECT_EQ(resolver_.cancelled[0], resolver_.asked[0].lookupId);

    SiteLookupFinished stopped;
    stopped.stopped = true;
    Answer(0, stopped);
    EXPECT_TRUE(observer_.answers.empty());
}

TEST_F(SiteLookupsTest, ALookupWaitsForItsProxyAndOneDroppedMeanwhileIsNeverAsked)
{
    proxy_.pending = true;
    LookUp(1);
    EXPECT_TRUE(resolver_.asked.empty());
    const DownloadId key = proxy_.questions.at(0).first;
    lookups_.OnProxyResolved(key, std::string("socks5://proxy.test:1080"));
    ASSERT_EQ(resolver_.asked.size(), 1u);
    EXPECT_EQ(resolver_.asked[0].proxy, "socks5://proxy.test:1080");

    LookUp(2, "https://www.youtube.com/watch?v=jNQXAC9IVRw");
    const DownloadId second = proxy_.questions.at(1).first;
    lookups_.Cancel(2);
    lookups_.OnProxyResolved(second, std::nullopt);
    EXPECT_EQ(resolver_.asked.size(), 1u);
    EXPECT_TRUE(resolver_.cancelled.empty()) << "it never reached the resolver";
}

TEST_F(SiteLookupsTest, PagesThatAreNotVideosAndTooManyLookupsAreRefusedAtOnce)
{
    LookUp(1, "http://localhost/video");  // never asked about
    ASSERT_EQ(observer_.answers.size(), 1u);
    EXPECT_EQ(observer_.answers[0].problem, Problem::Video);
    EXPECT_TRUE(resolver_.asked.empty());

    for (size_t i = 0; i < SiteLookups::kMaxPending; ++i) {
        std::string id = "video" + std::to_string(100000 + i);  // 11 characters
        LookUp(static_cast<std::int64_t>(10 + i), ("https://www.youtube.com/watch?v=" + id).c_str());
    }
    EXPECT_EQ(resolver_.asked.size(), SiteLookups::kMaxPending);
    LookUp(99, "https://www.youtube.com/watch?v=jNQXAC9IVRw");
    EXPECT_EQ(resolver_.asked.size(), SiteLookups::kMaxPending);
    EXPECT_EQ(observer_.answers.back().lookupId, 99);
    EXPECT_EQ(observer_.answers.back().problem, Problem::Busy);
}

TEST_F(SiteLookupsTest, ARefreshReusesOnlyAVeryRecentAnswer)
{
    LookUp(1);
    Answer(0, Found());
    clock_.now += std::chrono::seconds(30);
    lookups_.Refresh(DownloadId{5}, 2, kPage, "");
    ASSERT_EQ(refreshed_.size(), 1u) << "30 seconds old: the same addresses are still fresh";
    EXPECT_EQ(std::get<0>(refreshed_[0]), DownloadId{5});
    EXPECT_EQ(std::get<1>(refreshed_[0]), 2);
    EXPECT_EQ(resolver_.asked.size(), 1u);

    clock_.now += std::chrono::minutes(5);
    lookups_.Refresh(DownloadId{5}, 3, kPage, "");
    ASSERT_EQ(resolver_.asked.size(), 2u);
    LookUp(2);
    EXPECT_EQ(resolver_.asked.size(), 2u) << "the panel still takes the 5 minute old answer";
    EXPECT_EQ(observer_.answers.back().lookupId, 2);
    Answer(1, Found());
    ASSERT_EQ(refreshed_.size(), 2u);
    EXPECT_EQ(std::get<1>(refreshed_[1]), 3);
}

TEST_F(SiteLookupsTest, ACancelledRefreshIsNotAnswered)
{
    lookups_.Refresh(DownloadId{5}, 1, kPage, "");
    lookups_.CancelRefresh(DownloadId{5});
    ASSERT_EQ(resolver_.cancelled.size(), 1u);
    Answer(0, Found());
    EXPECT_TRUE(refreshed_.empty());
}

TEST_F(SiteLookupsTest, AnAnswerCanStartTheNextLookupOfTheSamePage)
{
    // The refresh hook asks again at once: the finished lookup is gone by then, so a new one
    // starts instead of the new request joining the lookup that just answered.
    react_ = [this](DownloadId id, std::int64_t epoch) {
        if (epoch == 1) {
            lookups_.Refresh(id, 2, kPage, "");
        }
    };
    lookups_.Refresh(DownloadId{5}, 1, kPage, "");
    SiteLookupFinished failed;
    failed.problem = Problem::Resolver;
    Answer(0, failed);
    ASSERT_EQ(resolver_.asked.size(), 2u);
    EXPECT_NE(resolver_.asked[1].lookupId, resolver_.asked[0].lookupId);
    Answer(1, Found());
    ASSERT_EQ(refreshed_.size(), 2u);
    EXPECT_TRUE(std::get<2>(refreshed_[1]).video);
}

TEST_F(SiteLookupsTest, ASignedInLookupIsAskedApartWithItsCookiesAndKeptApart)
{
    LookUpSiteVideoCommand signedIn{1, kPage};
    signedIn.cookies.push_back(ldm::model::Cookie{"youtube.com", false, "/", true, 0, "SID", "secret"});
    lookups_.LookUp(signedIn);
    LookUp(2);  // the same page without the sign-in: not joined to it
    ASSERT_EQ(resolver_.asked.size(), 2u);
    EXPECT_NE(resolver_.asked[0].cookies.find("SID\tsecret"), std::string::npos);
    EXPECT_TRUE(resolver_.asked[1].cookies.empty());

    Answer(0, Found());
    LookUp(3);  // the signed-in answer is not reused for a lookup without the sign-in
    EXPECT_EQ(resolver_.asked.size(), 2u) << "the unsigned lookup is still under way";
    Answer(1, Found());
    LookUp(4);
    EXPECT_EQ(resolver_.asked.size(), 2u) << "now each has its own answer kept";
    lookups_.Refresh(DownloadId{9}, 1, kPage, ".youtube.com\tTRUE\t/\tTRUE\t0\tSID\tsecret\n");
    EXPECT_EQ(resolver_.asked.size(), 2u) << "a very recent signed-in answer serves the refresh";
}

}  // namespace
