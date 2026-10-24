#include "grabber/GrabberExplorer.h"
#include "grabber/ProjectDefinition.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using ldm::core::FetchFinished;
using ldm::core::FetchRequest;
using ldm::grabber::GrabberExplorer;
using ldm::model::DownloadId;
using ldm::model::GrabberItemKind;
using ldm::model::GrabberItemState;
using ldm::model::GrabberProjectId;

class FakeGrabberStore final : public ldm::core::GrabberStore {
public:
    ldm::base::Result<std::vector<ldm::core::StoredGrabberProject>> LoadGrabberProjects() override
    {
        std::vector<ldm::core::StoredGrabberProject> rows;
        for (const auto& [id, row] : projects) {
            rows.push_back(row);
        }
        return rows;
    }
    ldm::base::Result<GrabberProjectId> InsertGrabberProject(const ldm::model::GrabberProject& project,
                                                             std::string_view json) override
    {
        const GrabberProjectId id{++lastId};
        auto stored = project;
        stored.id = id;
        stored.definition = {};
        projects[id.value] = {stored, std::string(json)};
        return id;
    }
    ldm::base::Status SaveGrabberProject(const ldm::model::GrabberProject& project, std::string_view json) override
    {
        auto stored = project;
        stored.definition = {};
        projects[project.id.value] = {stored, std::string(json)};
        return {};
    }
    ldm::base::Status DeleteGrabberProject(GrabberProjectId id) override
    {
        projects.erase(id.value);
        items.erase(id.value);
        return {};
    }
    ldm::base::Result<std::vector<ldm::model::GrabberItem>> LoadGrabberItems(GrabberProjectId id) override
    {
        std::vector<ldm::model::GrabberItem> rows;
        for (const auto& [itemId, item] : items[id.value]) {
            rows.push_back(item);
        }
        return rows;
    }
    ldm::base::Status SaveGrabberItems(GrabberProjectId id, std::span<const ldm::model::GrabberItem> rows) override
    {
        for (const auto& item : rows) {
            items[id.value][item.id] = item;
        }
        return {};
    }
    ldm::base::Status ClearGrabberItems(GrabberProjectId id) override
    {
        items.erase(id.value);
        return {};
    }

    std::map<std::int64_t, ldm::core::StoredGrabberProject> projects;
    std::map<std::int64_t, std::map<std::int64_t, ldm::model::GrabberItem>> items;
    std::int64_t lastId = 0;
};

class FakeEngine final : public ldm::core::TransferEngine {
public:
    void Start(ldm::core::TransferRequest) override {}
    void StartMedia(ldm::core::MediaRequest) override {}
    void ExtendMedia(DownloadId, std::int64_t, ldm::core::MediaExtension) override {}
    void Stop(DownloadId, std::int64_t) override {}
    void SetSpeedLimit(DownloadId, std::int64_t, std::optional<std::int64_t>) override {}
    void SetGlobalSpeedLimit(std::optional<std::int64_t>) override {}
    void Finalize(ldm::core::FinalizeRequest) override {}
    void Fetch(FetchRequest request) override { fetches.push_back(std::move(request)); }

    std::vector<FetchRequest> fetches;
};

class DirectProxy final : public ldm::core::ProxyResolver {
public:
    void Configure(const ldm::model::Settings&) override {}
    ldm::core::ProxyLookup Resolve(DownloadId, std::int64_t, const std::string&) override { return {}; }
};

class RecordingSink final : public ldm::core::CommandSink {
public:
    void Post(ldm::core::Command command) override { commands.push_back(std::move(command)); }

    template <typename T>
    std::vector<T> All() const
    {
        std::vector<T> result;
        for (const auto& command : commands) {
            if (const T* typed = std::get_if<T>(&command)) {
                result.push_back(*typed);
            }
        }
        return result;
    }

    std::vector<ldm::core::Command> commands;
};

class FakeFiles final : public ldm::core::FileSystem {
public:
    bool Exists(const fs::path& path) override { return written.contains(path.generic_string()); }
    std::optional<std::int64_t> FileSize(const fs::path&) override { return std::nullopt; }
    ldm::base::Status Remove(const fs::path&) override { return {}; }
    ldm::base::Status Recycle(const fs::path&) override { return {}; }
    ldm::base::Status CreateDirectories(const fs::path&) override { return {}; }
    ldm::base::Status RemoveFolder(const fs::path&) override { return {}; }
    ldm::base::Status WriteFile(const fs::path& path, std::string_view bytes, bool replace) override
    {
        if (!replace && Exists(path)) {
            return ldm::base::MakeError(ldm::base::ErrorCode::AlreadyExists, "exists");
        }
        written[path.generic_string()] = std::string(bytes);
        return {};
    }
    std::map<std::string, std::string> written;
};

class FakeSecrets final : public ldm::core::SecretProtector {
public:
    ldm::base::Result<std::vector<std::uint8_t>> Protect(std::string_view secret) override
    {
        std::vector<std::uint8_t> bytes{'P'};
        bytes.insert(bytes.end(), secret.begin(), secret.end());
        return bytes;
    }
    ldm::base::Result<std::string> Unprotect(std::span<const std::uint8_t> data) override
    {
        return std::string(data.begin() + 1, data.end());
    }
};

class FakeClock final : public ldm::core::Clock {
public:
    std::int64_t NowUnixMs() override { return unixMs; }
    std::chrono::steady_clock::time_point Steady() override { return steady; }
    void Advance(std::chrono::milliseconds by)
    {
        steady += by;
        unixMs += by.count();
    }
    std::int64_t unixMs = 1'800'000'000'000;
    std::chrono::steady_clock::time_point steady = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
};

class UtcZone final : public ldm::core::TimeZone {
public:
    std::int64_t LocalToUnixMs(std::int64_t localMs) const override { return localMs; }
    std::int64_t UnixToLocalMs(std::int64_t unixMs) const override { return unixMs; }
};

class FakeWakeTimer final : public ldm::core::WakeTimer {
public:
    void Arm(std::int64_t unixMs) override { armed = unixMs; }
    void Disarm() override { armed.reset(); }
    std::optional<std::int64_t> armed;
};

class RecordingObserver final : public ldm::grabber::GrabberObserver {
public:
    void OnGrabberSnapshot(std::shared_ptr<const ldm::grabber::GrabberSnapshot> value) override { snapshot = value; }
    void OnGrabberNotification(ldm::grabber::GrabberNotification value) override { notifications.push_back(value); }

    template <typename T>
    std::vector<T> All() const
    {
        std::vector<T> result;
        for (const auto& notification : notifications) {
            if (const T* typed = std::get_if<T>(&notification)) {
                result.push_back(*typed);
            }
        }
        return result;
    }

    std::shared_ptr<const ldm::grabber::GrabberSnapshot> snapshot;
    std::vector<ldm::grabber::GrabberNotification> notifications;
};

// A web page of the fake site, or a file.
struct Resource {
    std::string contentType;
    std::string body;
    std::optional<std::int64_t> length;  // announced; the body's size when not set
};

class GrabberExplorerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ports_.store = &store_;
        ports_.engine = &engine_;
        ports_.proxy = &proxy_;
        ports_.downloads = &sink_;
        ports_.fileSystem = &files_;
        ports_.secrets = &secrets_;
        ports_.clock = &clock_;
        ports_.timeZone = &zone_;
        ports_.wakeTimer = &wake_;
        ports_.observer = &observer_;
    }

    void Start(ldm::grabber::ExplorerConfig config = {})
    {
        explorer_ = std::make_unique<GrabberExplorer>(ports_, config);
        explorer_->Initialize();
        auto core = std::make_shared<ldm::core::Snapshot>();
        core->settings.userAgent = "LDM-Test";
        ldm::model::Category general;
        general.id = ldm::model::CategoryId{1};
        general.name = "General";
        general.builtin = true;
        general.folder = "C:\\Downloads";
        core->categories.push_back(general);
        core_ = core;
        explorer_->OnCoreSnapshot(core_);
    }

    GrabberProjectId Save(ldm::model::GrabberProjectDefinition definition, std::string name = "Site")
    {
        ldm::core::GrabberSaveProjectCommand save;
        save.token = ++token_;
        save.project.name = std::move(name);
        save.project.definition = std::move(definition);
        explorer_->Handle(std::move(save));
        const auto saved = observer_.All<ldm::grabber::GrabberProjectSaved>();
        EXPECT_FALSE(saved.empty());
        EXPECT_FALSE(saved.back().error) << *saved.back().error;
        return saved.back().id.value_or(GrabberProjectId{});
    }

    // Answers every fetch from the site table until none is left.
    void Serve()
    {
        for (int guard = 0; guard < 100'000 && !engine_.fetches.empty(); ++guard) {
            FetchRequest request = std::move(engine_.fetches.front());
            engine_.fetches.erase(engine_.fetches.begin());
            served_.push_back(request);
            FetchFinished finished;
            finished.fetchId = request.fetchId;
            finished.finalUrl = request.url;
            const auto found = site_.find(request.url);
            if (found == site_.end()) {
                finished.failure = ldm::core::TransferFailure{ldm::core::FailureReason::NotFound,
                                                              ldm::core::FailureClass::Fatal, 404, "not found"};
            } else {
                const Resource& resource = found->second;
                finished.contentType = resource.contentType;
                finished.contentLength = resource.length.value_or(static_cast<std::int64_t>(resource.body.size()));
                const bool wanted =
                    request.bodyTypes.empty() || std::find(request.bodyTypes.begin(), request.bodyTypes.end(),
                                                           resource.contentType) != request.bodyTypes.end();
                if (request.headOnly) {
                    // no body
                } else if (!wanted) {
                    finished.bodySkipped = true;
                } else {
                    finished.body = resource.body;
                }
            }
            explorer_->OnEngineEvent(ldm::core::EngineEvent{{}, 0, finished});
        }
    }

    void Page(const std::string& url, const std::string& html) { site_[url] = Resource{"text/html", html, {}}; }
    void File(const std::string& url, const std::string& type, std::int64_t size)
    {
        site_[url] = Resource{type, std::string(), size};
    }

    std::vector<ldm::model::GrabberItem> Items(GrabberProjectId id)
    {
        explorer_->Handle(ldm::core::GrabberOpenCommand{id, true});
        clock_.Advance(std::chrono::seconds(1));
        explorer_->Tick();
        std::vector<ldm::model::GrabberItem> items;
        for (const auto& project : observer_.snapshot->projects) {
            if (project.project.id == id) {
                for (const auto& item : project.items) {
                    items.push_back(*item);
                }
            }
        }
        return items;
    }

    const ldm::grabber::GrabberProjectView* View(GrabberProjectId id)
    {
        clock_.Advance(std::chrono::seconds(1));
        explorer_->Tick();
        for (const auto& project : observer_.snapshot->projects) {
            if (project.project.id == id) {
                return &project;
            }
        }
        return nullptr;
    }

    static const ldm::model::GrabberItem* ByUrl(const std::vector<ldm::model::GrabberItem>& items, std::string_view url)
    {
        const auto it = std::find_if(items.begin(), items.end(), [url](const auto& item) { return item.url == url; });
        return it == items.end() ? nullptr : &*it;
    }

    std::vector<std::string> ServedUrls() const
    {
        std::vector<std::string> urls;
        for (const auto& request : served_) {
            urls.push_back(request.url);
        }
        return urls;
    }

    void Complete(std::uint64_t token, std::int64_t downloadId)
    {
        ldm::core::AddResult added;
        added.token = token;
        added.id = DownloadId{downloadId};
        EXPECT_TRUE(explorer_->TakeNotification(added));
    }

    FakeGrabberStore store_;
    FakeEngine engine_;
    DirectProxy proxy_;
    RecordingSink sink_;
    FakeFiles files_;
    FakeSecrets secrets_;
    FakeClock clock_;
    UtcZone zone_;
    FakeWakeTimer wake_;
    RecordingObserver observer_;
    ldm::grabber::ExplorerPorts ports_;
    std::unique_ptr<GrabberExplorer> explorer_;
    std::shared_ptr<const ldm::core::Snapshot> core_;
    std::map<std::string, Resource> site_;
    std::vector<FetchRequest> served_;
    std::uint64_t token_ = 0;
};

ldm::model::GrabberProjectDefinition Definition(std::string start)
{
    ldm::model::GrabberProjectDefinition definition;
    definition.startPage = std::move(start);
    definition.explore.baseSiteDepth = 1;
    definition.explore.noParentDirectories = false;
    return definition;
}

TEST_F(GrabberExplorerTest, ExploresBreadthFirstWithinTheDepth)
{
    Start();
    Page("https://example.com/", R"(<a href="a.html">Page A</a> <a href="/b.html">B</a> <a href="/">Home</a>
        <img src="logo.png" alt="Logo"> <a href="files/report.pdf">The report</a> <a href="javascript:go">J</a>
        <a href="http://192.168.1.1/admin.zip">Router</a> <a href="https://other.com/page.html">Other</a>)");
    Page("https://example.com/a.html", R"(<a href="deep.html">Deep</a><a href="a.zip">Zip</a><a href="/">Back</a>)");
    Page("https://example.com/b.html", R"(<frame src="frame.html">)");
    Page("https://example.com/deep.html", "never read");
    const GrabberProjectId id = Save(Definition("https://example.com/"));

    explorer_->Handle(ldm::core::GrabberExploreCommand{id, false, {}});
    Serve();

    const std::vector<std::string> expected{"https://example.com/", "https://example.com/a.html",
                                            "https://example.com/b.html"};
    EXPECT_EQ(ServedUrls(), expected);
    EXPECT_EQ(served_[1].referer, "https://example.com/");
    EXPECT_EQ(served_[0].userAgent, "LDM-Test");
    EXPECT_TRUE(served_[0].refusePrivateAddresses);
    EXPECT_EQ(served_[0].maxBytes, 8u * 1024 * 1024);

    const auto items = Items(id);
    ASSERT_TRUE(ByUrl(items, "https://example.com/"));
    EXPECT_EQ(ByUrl(items, "https://example.com/")->state, GrabberItemState::Done);
    const auto* logo = ByUrl(items, "https://example.com/logo.png");
    ASSERT_TRUE(logo);
    EXPECT_EQ(logo->kind, GrabberItemKind::File);
    EXPECT_EQ(logo->linkText, "Logo");
    EXPECT_TRUE(logo->checked);
    const auto* report = ByUrl(items, "https://example.com/files/report.pdf");
    ASSERT_TRUE(report);
    EXPECT_EQ(report->linkText, "The report");
    EXPECT_EQ(report->depth, 1);
    const auto* zip = ByUrl(items, "https://example.com/a.zip");
    ASSERT_TRUE(zip);
    EXPECT_EQ(zip->depth, 2);
    const auto* deep = ByUrl(items, "https://example.com/deep.html");
    ASSERT_TRUE(deep);
    EXPECT_EQ(deep->state, GrabberItemState::Found);  // listed, not read: beyond the depth
    EXPECT_TRUE(ByUrl(items, "https://example.com/frame.html"));
    EXPECT_TRUE(ByUrl(items, "https://other.com/page.html"));
    EXPECT_FALSE(ByUrl(items, "http://192.168.1.1/admin.zip")) << "a public site cannot lead into the home network";
    EXPECT_EQ(std::count_if(items.begin(), items.end(), [](const auto& item) { return item.url == "https://example.com/"; }),
              1);

    const auto finished = observer_.All<ldm::grabber::GrabberRunFinished>();
    ASSERT_EQ(finished.size(), 1u);
    EXPECT_TRUE(finished[0].explored);
    const auto* view = View(id);
    ASSERT_TRUE(view);
    EXPECT_FALSE(view->exploring);
    EXPECT_EQ(view->stats.pagesExplored, 3);
    EXPECT_TRUE(view->project.lastRunAtMs);
    EXPECT_FALSE(view->startPageFailure);
}

TEST_F(GrabberExplorerTest, SaysWhenTheStartPageCannotBeRead)
{
    Start();
    const GrabberProjectId id = Save(Definition("https://example.com/missing.html"));
    explorer_->Handle(ldm::core::GrabberExploreCommand{id, false, {}});
    Serve();
    const auto* view = View(id);
    ASSERT_TRUE(view);
    ASSERT_TRUE(view->startPageFailure);
    EXPECT_FALSE(view->startPageFailure->empty());
    Page("https://example.com/missing.html", "<p>back</p>");
    explorer_->Handle(ldm::core::GrabberExploreCommand{id, false, {}});
    Serve();
    EXPECT_FALSE(View(id)->startPageFailure);
}

TEST_F(GrabberExplorerTest, ReadsTheWholeSiteWithoutLooping)
{
    Start();
    Page("http://localhost/", R"(<a href="1.html">1</a>)");
    Page("http://localhost/1.html", R"(<a href="2.html">2</a><a href="/">0</a>)");
    Page("http://localhost/2.html", R"(<a href="1.html">1</a><a href="http://192.168.1.1/x.jpg">x</a>)");
    auto definition = Definition("http://localhost/");
    definition.explore.baseSiteDepth = std::nullopt;
    const GrabberProjectId id = Save(definition);
    explorer_->Handle(ldm::core::GrabberExploreCommand{id, false, {}});
    Serve();
    EXPECT_EQ(ServedUrls().size(), 3u);
    EXPECT_FALSE(served_[0].refusePrivateAddresses);
    EXPECT_TRUE(ByUrl(Items(id), "http://192.168.1.1/x.jpg")) << "a site in the home network may link into it";
}

TEST_F(GrabberExplorerTest, StopsAtThePageCap)
{
    ldm::grabber::ExplorerConfig config;
    config.maxPagesExplored = 2;
    Start(config);
    std::string links;
    for (int i = 0; i < 10; ++i) {
        links += "<a href=\"p" + std::to_string(i) + ".html\">p</a>";
        Page("https://example.com/p" + std::to_string(i) + ".html", "<p>x</p>");
    }
    Page("https://example.com/", links);
    const GrabberProjectId id = Save(Definition("https://example.com/"));
    explorer_->Handle(ldm::core::GrabberExploreCommand{id, false, {}});
    Serve();
    const auto* view = View(id);
    ASSERT_TRUE(view);
    EXPECT_TRUE(view->stats.limitReached);
    EXPECT_LE(view->stats.pagesExplored, 2 + 4);  // the cap, plus pages already being read
}

TEST_F(GrabberExplorerTest, KeepsToTheExploreConcurrency)
{
    Start();
    std::string links;
    for (int i = 0; i < 10; ++i) {
        links += "<a href=\"p" + std::to_string(i) + ".html\">p</a>";
    }
    Page("https://example.com/", links);
    auto definition = Definition("https://example.com/");
    definition.exploreConcurrency = 3;
    const GrabberProjectId id = Save(definition);
    explorer_->Handle(ldm::core::GrabberExploreCommand{id, false, {}});
    ASSERT_EQ(engine_.fetches.size(), 1u);
    FetchRequest start = engine_.fetches.front();
    engine_.fetches.clear();
    FetchFinished finished;
    finished.fetchId = start.fetchId;
    finished.finalUrl = start.url;
    finished.contentType = "text/html";
    finished.body = links;
    explorer_->OnEngineEvent(ldm::core::EngineEvent{{}, 0, finished});
    EXPECT_EQ(engine_.fetches.size(), 3u);
    for (const auto& fetch : engine_.fetches) {
        EXPECT_LE(fetch.fetchId, ldm::core::kGrabberFetchIdBase);
    }
}

TEST_F(GrabberExplorerTest, TreatsAPageThatIsNotHtmlAsAFile)
{
    Start();
    Page("https://example.com/", R"(<a href="download">Get it</a>)");
    File("https://example.com/download", "application/zip", 5000);
    const GrabberProjectId id = Save(Definition("https://example.com/"));
    explorer_->Handle(ldm::core::GrabberExploreCommand{id, false, {}});
    Serve();
    const auto items = Items(id);
    const auto* file = ByUrl(items, "https://example.com/download");
    ASSERT_TRUE(file);
    EXPECT_EQ(file->kind, GrabberItemKind::File);
    EXPECT_EQ(file->size, 5000);
    EXPECT_EQ(file->contentType, "application/zip");
    EXPECT_TRUE(file->checked);
}

TEST_F(GrabberExplorerTest, AsksForSizesTheFiltersNeed)
{
    Start();
    Page("https://example.com/", R"(<a href="small.zip">s</a><a href="big.zip">b</a><a href="ok.zip">o</a>)");
    File("https://example.com/small.zip", "application/zip", 10);
    File("https://example.com/big.zip", "application/zip", 10'000);
    File("https://example.com/ok.zip", "application/zip", 500);
    auto definition = Definition("https://example.com/");
    definition.files.minBytes = 100;
    definition.files.maxBytes = 1000;
    const GrabberProjectId id = Save(definition);
    explorer_->Handle(ldm::core::GrabberExploreCommand{id, false, {}});
    Serve();
    const auto heads = std::count_if(served_.begin(), served_.end(), [](const auto& r) { return r.headOnly; });
    EXPECT_EQ(heads, 3);
    const auto items = Items(id);
    EXPECT_EQ(ByUrl(items, "https://example.com/small.zip")->state, GrabberItemState::Filtered);
    EXPECT_EQ(ByUrl(items, "https://example.com/big.zip")->state, GrabberItemState::Filtered);
    EXPECT_TRUE(ByUrl(items, "https://example.com/ok.zip")->checked);
}

TEST_F(GrabberExplorerTest, DownloadsCheckedFilesThroughTheCoordinator)
{
    Start();
    Page("https://example.com/", R"(<a href="a.zip">First file</a><a href="b.zip">Second</a><a href="c.zip">Third</a>)");
    auto definition = Definition("https://example.com/");
    definition.downloadConcurrency = 2;
    const GrabberProjectId id = Save(definition);
    explorer_->Handle(ldm::core::GrabberExploreCommand{id, false, {}});
    Serve();
    auto items = Items(id);
    explorer_->Handle(ldm::core::GrabberCheckCommand{id, {ByUrl(items, "https://example.com/c.zip")->id}, false, false});
    explorer_->Handle(ldm::core::GrabberDownloadCommand{id});

    auto adds = sink_.All<ldm::core::AddDownloadCommand>();
    ASSERT_EQ(adds.size(), 2u);
    EXPECT_EQ(adds[0].url, "https://example.com/a.zip");
    EXPECT_EQ(adds[0].projectId, id);
    EXPECT_FALSE(adds[0].listed);
    EXPECT_EQ(adds[0].referer, "https://example.com/");
    EXPECT_EQ(adds[0].description, "First file");
    EXPECT_EQ(adds[0].duplicatePolicy, ldm::model::DuplicatePolicy::AddNumbered);
    EXPECT_GE(adds[0].token, GrabberExplorer::kTokenBase);

    Complete(adds[0].token, 100);
    Complete(adds[1].token, 101);
    EXPECT_FALSE(explorer_->TakeNotification(ldm::core::AddResult{5, DownloadId{7}, {}, {}, false}))
        << "the front end's own adds are not the Grabber's";

    // The core's list shows them hidden.
    auto core = std::make_shared<ldm::core::Snapshot>(*core_);
    for (const std::int64_t downloadId : {100, 101}) {
        auto view = std::make_shared<ldm::core::DownloadView>();
        view->download.id = DownloadId{downloadId};
        view->download.projectId = id;
        view->download.listed = false;
        view->download.status = ldm::model::DownloadStatus::Downloading;
        core->downloads.push_back(view);
    }
    explorer_->OnCoreSnapshot(core);

    ldm::core::DownloadCompleted done;
    done.id = DownloadId{100};
    done.finalPath = "C:\\Downloads\\a.zip";
    done.size = 1234;
    done.inMainList = false;
    EXPECT_TRUE(explorer_->TakeNotification(done)) << "the front end does not announce a hidden download";
    items = Items(id);
    EXPECT_EQ(ByUrl(items, "https://example.com/a.zip")->state, GrabberItemState::Done);
    EXPECT_EQ(ByUrl(items, "https://example.com/a.zip")->localPath, "C:\\Downloads\\a.zip");
    EXPECT_EQ(ByUrl(items, "https://example.com/a.zip")->size, 1234);
    EXPECT_EQ(ByUrl(items, "https://example.com/c.zip")->state, GrabberItemState::Found);

    // One more attempt after a failure, then the file shows failed.
    ldm::core::DownloadFailed failed;
    failed.id = DownloadId{101};
    failed.inMainList = false;
    EXPECT_TRUE(explorer_->TakeNotification(failed));
    const auto resumes = sink_.All<ldm::core::ResumeCommand>();
    ASSERT_EQ(resumes.size(), 1u);
    EXPECT_EQ(resumes[0].ids, std::vector<DownloadId>{DownloadId{101}});
    EXPECT_TRUE(explorer_->TakeNotification(failed));
    items = Items(id);
    EXPECT_EQ(ByUrl(items, "https://example.com/b.zip")->state, GrabberItemState::Failed);
    EXPECT_FALSE(View(id)->downloading);
    const auto finished = observer_.All<ldm::grabber::GrabberRunFinished>();
    ASSERT_EQ(finished.size(), 2u);
    EXPECT_FALSE(finished[1].explored);

    explorer_->Handle(ldm::core::GrabberAddToMainListCommand{id, {}});
    const auto listed = sink_.All<ldm::core::SetListedCommand>();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed[0].ids, (std::vector<DownloadId>{DownloadId{100}, DownloadId{101}}));
}

TEST_F(GrabberExplorerTest, StopsItsDownloads)
{
    Start();
    Page("https://example.com/", R"(<a href="a.zip">a</a>)");
    const GrabberProjectId id = Save(Definition("https://example.com/"));
    explorer_->Handle(ldm::core::GrabberExploreCommand{id, false, {}});
    Serve();
    explorer_->Handle(ldm::core::GrabberDownloadCommand{id});
    const auto adds = sink_.All<ldm::core::AddDownloadCommand>();
    ASSERT_EQ(adds.size(), 1u);
    Complete(adds[0].token, 100);
    explorer_->Handle(ldm::core::GrabberStopDownloadsCommand{id});
    const auto stops = sink_.All<ldm::core::StopCommand>();
    ASSERT_EQ(stops.size(), 1u);
    EXPECT_EQ(stops[0].ids, std::vector<DownloadId>{DownloadId{100}});
    EXPECT_EQ(ByUrl(Items(id), "https://example.com/a.zip")->state, GrabberItemState::Found);
}

TEST_F(GrabberExplorerTest, SavesPagesForOfflineBrowsing)
{
    Start();
    Page("https://example.com/", R"(<a href="docs/a.html">A</a><img src="img/logo.png"><a href="https://other.com/x">X</a>)");
    Page("https://example.com/docs/a.html", R"(<a href="../">Home</a><img src="../img/logo.png">)");
    auto definition = ldm::grabber::ApplyTemplate(Definition("https://example.com/"), ldm::model::GrabberTemplate::WholeSite);
    definition.saveMode = ldm::model::GrabberSaveMode::OneFolder;
    definition.folder = "C:\\Mirror";
    const GrabberProjectId id = Save(definition);
    explorer_->Handle(ldm::core::GrabberExploreCommand{id, false, {}});
    Serve();

    ASSERT_EQ(files_.written.size(), 2u);
    const std::string start = files_.written.at("C:/Mirror/example.com/index.html");
    EXPECT_EQ(start, R"(<a href="docs/a.html">A</a><img src="img/logo.png"><a href="https://other.com/x">X</a>)");
    const std::string page = files_.written.at("C:/Mirror/example.com/docs/a.html");
    EXPECT_EQ(page, R"(<a href="../index.html">Home</a><img src="../img/logo.png">)");

    explorer_->Handle(ldm::core::GrabberDownloadCommand{id});
    const auto adds = sink_.All<ldm::core::AddDownloadCommand>();
    ASSERT_EQ(adds.size(), 1u);
    EXPECT_EQ(adds[0].saveDir, "C:\\Mirror\\example.com\\img");
    EXPECT_EQ(adds[0].fileName, "logo.png");
}

TEST_F(GrabberExplorerTest, ASavedPageNoLongerTakesItsBaseFromTheWebsite)
{
    Start();
    Page("https://example.com/", R"(<base href="https://example.com/"><a href="docs/a.html">A</a>)");
    Page("https://example.com/docs/a.html", R"(<p>A</p>)");
    auto definition = ldm::grabber::ApplyTemplate(Definition("https://example.com/"), ldm::model::GrabberTemplate::WholeSite);
    definition.saveMode = ldm::model::GrabberSaveMode::OneFolder;
    definition.folder = "C:\\Mirror";
    const GrabberProjectId id = Save(definition);
    explorer_->Handle(ldm::core::GrabberExploreCommand{id, false, {}});
    Serve();
    const std::string start = files_.written.at("C:/Mirror/example.com/index.html");
    EXPECT_EQ(start, R"(<base href="index.html"><a href="docs/a.html">A</a>)");
}

TEST_F(GrabberExplorerTest, ValidatesProjects)
{
    Start();
    const GrabberProjectId first = Save(Definition("https://example.com/"), "Pictures");
    ldm::core::GrabberSaveProjectCommand duplicate;
    duplicate.token = 99;
    duplicate.project.name = "  pictures ";
    duplicate.project.definition = Definition("https://example.com/");
    explorer_->Handle(duplicate);
    auto saved = observer_.All<ldm::grabber::GrabberProjectSaved>();
    EXPECT_EQ(saved.back().token, 99u);
    EXPECT_TRUE(saved.back().error);

    ldm::core::GrabberSaveProjectCommand bad;
    bad.token = 100;
    bad.project.name = "Bad";
    bad.project.definition = Definition("file:///C:/Windows");
    explorer_->Handle(bad);
    saved = observer_.All<ldm::grabber::GrabberProjectSaved>();
    EXPECT_TRUE(saved.back().error);

    ldm::core::GrabberSaveProjectCommand login;
    login.token = 101;
    login.project.id = first;
    login.project.name = "Pictures";
    login.project.loginUser = "me";
    login.project.definition = Definition("https://example.com/");
    login.loginPassword = "secret";
    explorer_->Handle(login);
    EXPECT_EQ(store_.projects.at(first.value).project.loginSecret.size(), 7u);
    const auto* view = View(first);
    ASSERT_TRUE(view);
    EXPECT_TRUE(view->hasPassword);
    EXPECT_TRUE(view->project.loginSecret.empty());

    // The credentials go to the base site only.
    Page("https://example.com/", R"(<a href="https://cdn.example.com/a.html">cdn</a>)");
    explorer_->Handle(ldm::core::GrabberExploreCommand{first, false, {}});
    ASSERT_EQ(engine_.fetches.size(), 1u);
    ASSERT_TRUE(engine_.fetches[0].credentials);
    EXPECT_EQ(engine_.fetches[0].credentials->password, "secret");
}

TEST_F(GrabberExplorerTest, ReadsBackDamagedProjectsWithoutRunningThem)
{
    ldm::model::GrabberProject project;
    project.id = GrabberProjectId{5};
    project.name = "Broken";
    store_.projects[5] = {project, "{not json"};
    Start();
    const auto* view = View(GrabberProjectId{5});
    ASSERT_TRUE(view);
    EXPECT_TRUE(view->damaged);
    explorer_->Handle(ldm::core::GrabberExploreCommand{GrabberProjectId{5}, false, {}});
    EXPECT_TRUE(engine_.fetches.empty());
    EXPECT_FALSE(observer_.All<ldm::grabber::GrabberError>().empty());
}

TEST_F(GrabberExplorerTest, UpdateAllKeepsChecks)
{
    Start();
    Page("https://example.com/", R"(<a href="a.zip">a</a><a href="b.zip">b</a>)");
    const GrabberProjectId id = Save(Definition("https://example.com/"));
    explorer_->Handle(ldm::core::GrabberExploreCommand{id, false, {}});
    Serve();
    auto items = Items(id);
    explorer_->Handle(ldm::core::GrabberCheckCommand{id, {ByUrl(items, "https://example.com/a.zip")->id}, false, false});
    Page("https://example.com/", R"(<a href="a.zip">a</a><a href="b.zip">b</a><a href="c.zip">c</a>)");
    explorer_->Handle(ldm::core::GrabberExploreCommand{id, true, {}});
    Serve();
    items = Items(id);
    EXPECT_FALSE(ByUrl(items, "https://example.com/a.zip")->checked);
    EXPECT_TRUE(ByUrl(items, "https://example.com/b.zip")->checked);
    EXPECT_TRUE(ByUrl(items, "https://example.com/c.zip")->checked);
    EXPECT_EQ(items.size(), 4u);

    explorer_->Handle(ldm::core::GrabberExploreCommand{id, false, {}});
    Serve();
    items = Items(id);
    EXPECT_TRUE(ByUrl(items, "https://example.com/a.zip")->checked) << "a new exploration starts over";
}

TEST_F(GrabberExplorerTest, RunsProjectSchedules)
{
    Start();
    Page("https://example.com/", R"(<a href="a.zip">a</a>)");
    const GrabberProjectId id = Save(Definition("https://example.com/"));
    ldm::core::GrabberSaveProjectCommand scheduled;
    scheduled.token = 50;
    scheduled.project.id = id;
    scheduled.project.name = "Site";
    scheduled.project.definition = Definition("https://example.com/");
    scheduled.project.scheduleMode = ldm::model::GrabberScheduleMode::ExploreDownload;
    scheduled.project.schedule.enabled = true;
    scheduled.project.schedule.mode = ldm::model::ScheduleMode::Daily;
    const std::int64_t minuteOfDay = (clock_.unixMs % (24 * 3'600'000)) / 60'000;
    scheduled.project.schedule.startMinutes = static_cast<int>((minuteOfDay + 2) % (24 * 60));

    // An open window keeps the schedule from starting it.
    explorer_->Handle(scheduled);
    const std::int64_t dayStart = clock_.unixMs - clock_.unixMs % (24 * 3'600'000);
    ASSERT_TRUE(wake_.armed) << "the computer wakes for the start";
    EXPECT_EQ(*wake_.armed % 60'000, 0);
    EXPECT_GE(*wake_.armed, dayStart);
    explorer_->Handle(ldm::core::GrabberOpenCommand{id, true});
    clock_.Advance(std::chrono::minutes(3));
    explorer_->Tick();
    EXPECT_TRUE(engine_.fetches.empty());
    explorer_->Handle(ldm::core::GrabberOpenCommand{id, false});

    clock_.Advance(std::chrono::hours(24));
    explorer_->Tick();
    ASSERT_EQ(engine_.fetches.size(), 1u);
    Serve();
    EXPECT_EQ(sink_.All<ldm::core::AddDownloadCommand>().size(), 1u) << "explore and download";
    const auto finished = observer_.All<ldm::grabber::GrabberRunFinished>();
    ASSERT_FALSE(finished.empty());
    EXPECT_TRUE(finished[0].scheduled);
}

TEST_F(GrabberExplorerTest, AScheduleWhoseStopMeetsItsStartStillRuns)
{
    Start();
    Page("https://example.com/", R"(<a href="a.zip">a</a>)");
    const GrabberProjectId id = Save(Definition("https://example.com/"));
    ldm::core::GrabberSaveProjectCommand scheduled;
    scheduled.token = 50;
    scheduled.project.id = id;
    scheduled.project.name = "Site";
    scheduled.project.definition = Definition("https://example.com/");
    scheduled.project.scheduleMode = ldm::model::GrabberScheduleMode::Explore;
    scheduled.project.schedule.enabled = true;
    scheduled.project.schedule.mode = ldm::model::ScheduleMode::Daily;
    const std::int64_t minuteOfDay = (clock_.unixMs % (24 * 3'600'000)) / 60'000;
    scheduled.project.schedule.startMinutes = static_cast<int>((minuteOfDay + 2) % (24 * 60));
    scheduled.project.schedule.stopEnabled = true;
    scheduled.project.schedule.stopMinutes = scheduled.project.schedule.startMinutes;
    explorer_->Handle(scheduled);
    clock_.Advance(std::chrono::minutes(3));
    explorer_->Tick();
    EXPECT_EQ(engine_.fetches.size(), 1u) << "the stop ends the next day's run, not this one";
}

TEST_F(GrabberExplorerTest, DeletingAProjectRemovesItsHiddenDownloads)
{
    Start();
    const GrabberProjectId id = Save(Definition("https://example.com/"));
    auto core = std::make_shared<ldm::core::Snapshot>(*core_);
    auto hidden = std::make_shared<ldm::core::DownloadView>();
    hidden->download.id = DownloadId{9};
    hidden->download.projectId = id;
    hidden->download.listed = false;
    auto shown = std::make_shared<ldm::core::DownloadView>();
    shown->download.id = DownloadId{10};
    shown->download.projectId = id;
    shown->download.listed = true;
    core->downloads = {hidden, shown};
    explorer_->OnCoreSnapshot(core);
    explorer_->Handle(ldm::core::GrabberDeleteProjectCommand{id});
    const auto deletes = sink_.All<ldm::core::DeleteCommand>();
    ASSERT_EQ(deletes.size(), 1u);
    EXPECT_EQ(deletes[0].ids, std::vector<DownloadId>{DownloadId{9}});
    EXPECT_FALSE(deletes[0].deleteFiles);
    EXPECT_FALSE(store_.projects.contains(id.value));
}

TEST_F(GrabberExplorerTest, DropsAnswersOfAStoppedExploration)
{
    Start();
    Page("https://example.com/", R"(<a href="a.html">a</a><a href="b.zip">b</a>)");
    Page("https://example.com/a.html", R"(<a href="old.zip">old</a>)");
    const GrabberProjectId id = Save(Definition("https://example.com/"));
    explorer_->Handle(ldm::core::GrabberExploreCommand{id, false, {}});
    // Read the start page only; a.html is being fetched when the user stops and explores again.
    FetchRequest first = engine_.fetches.front();
    engine_.fetches.erase(engine_.fetches.begin());
    FetchFinished start;
    start.fetchId = first.fetchId;
    start.finalUrl = first.url;
    start.contentType = "text/html";
    start.body = site_.at(first.url).body;
    explorer_->OnEngineEvent(ldm::core::EngineEvent{{}, 0, start});
    ASSERT_EQ(engine_.fetches.size(), 1u);
    const FetchRequest stale = engine_.fetches.front();
    engine_.fetches.clear();
    explorer_->Handle(ldm::core::GrabberStopCommand{id});
    Page("https://example.com/", R"(<a href="new.zip">new</a>)");
    explorer_->Handle(ldm::core::GrabberExploreCommand{id, false, {}});

    // The old answer for a.html arrives now; in the new list its item id belongs to new.zip.
    FetchFinished late;
    late.fetchId = stale.fetchId;
    late.finalUrl = stale.url;
    late.contentType = "text/html";
    late.body = site_.at(stale.url).body;
    explorer_->OnEngineEvent(ldm::core::EngineEvent{{}, 0, late});
    Serve();
    const auto items = Items(id);
    EXPECT_FALSE(ByUrl(items, "https://example.com/old.zip"));
    ASSERT_TRUE(ByUrl(items, "https://example.com/new.zip"));
    EXPECT_EQ(ByUrl(items, "https://example.com/new.zip")->state, GrabberItemState::Found);
    EXPECT_FALSE(View(id)->exploring);
}

TEST_F(GrabberExplorerTest, DownloadingWhileExploringEndsWithTheExploration)
{
    Start();
    Page("https://example.com/", R"(<a href="a.zip">a</a><a href="p.html">p</a>)");
    Page("https://example.com/p.html", "<p>no files</p>");
    auto definition = Definition("https://example.com/");
    definition.files.downloadImmediately = true;
    const GrabberProjectId id = Save(definition);
    explorer_->Handle(ldm::core::GrabberExploreCommand{id, false, {}});
    // The start page lists a.zip, which starts downloading while p.html is still to be read.
    FetchRequest first = engine_.fetches.front();
    engine_.fetches.erase(engine_.fetches.begin());
    FetchFinished start;
    start.fetchId = first.fetchId;
    start.finalUrl = first.url;
    start.contentType = "text/html";
    start.body = site_.at(first.url).body;
    explorer_->OnEngineEvent(ldm::core::EngineEvent{{}, 0, start});
    const auto adds = sink_.All<ldm::core::AddDownloadCommand>();
    ASSERT_EQ(adds.size(), 1u);
    Complete(adds[0].token, 100);
    ldm::core::DownloadCompleted done;
    done.id = DownloadId{100};
    done.inMainList = false;
    EXPECT_TRUE(explorer_->TakeNotification(done));
    EXPECT_TRUE(View(id)->downloading) << "more files may come while exploring";
    Serve();
    EXPECT_FALSE(View(id)->exploring);
    EXPECT_FALSE(View(id)->downloading) << "nothing is left once exploring ended";
}

TEST_F(GrabberExplorerTest, ADownloadAddedBeforeAStopIsUsedOnceDownloadingStartsAgain)
{
    Start();
    Page("https://example.com/", R"(<a href="a.zip">a</a>)");
    const GrabberProjectId id = Save(Definition("https://example.com/"));
    explorer_->Handle(ldm::core::GrabberExploreCommand{id, false, {}});
    Serve();
    explorer_->Handle(ldm::core::GrabberDownloadCommand{id});
    ASSERT_EQ(sink_.All<ldm::core::AddDownloadCommand>().size(), 1u);
    const std::uint64_t token = sink_.All<ldm::core::AddDownloadCommand>()[0].token;
    explorer_->Handle(ldm::core::GrabberStopDownloadsCommand{id});
    explorer_->Handle(ldm::core::GrabberDownloadCommand{id});
    EXPECT_EQ(sink_.All<ldm::core::AddDownloadCommand>().size(), 1u) << "the add on its way serves the file";
    Complete(token, 100);
    EXPECT_TRUE(sink_.All<ldm::core::StopCommand>().empty());
    EXPECT_EQ(ByUrl(Items(id), "https://example.com/a.zip")->downloadId, DownloadId{100});
    EXPECT_EQ(ByUrl(Items(id), "https://example.com/a.zip")->state, GrabberItemState::Downloading);

    // Stopped for good while an add is on its way: the answer stops the download it made.
    explorer_->Handle(ldm::core::GrabberStopDownloadsCommand{id});
    auto core = std::make_shared<ldm::core::Snapshot>(*core_);
    explorer_->OnCoreSnapshot(core);
    explorer_->Handle(ldm::core::GrabberOpenCommand{id, false});
    Page("https://example.com/", R"(<a href="b.zip">b</a>)");
    explorer_->Handle(ldm::core::GrabberExploreCommand{id, true, {}});
    Serve();
    explorer_->Handle(
        ldm::core::GrabberCheckCommand{id, {ByUrl(Items(id), "https://example.com/a.zip")->id}, false, false});
    explorer_->Handle(ldm::core::GrabberDownloadCommand{id});
    const auto adds = sink_.All<ldm::core::AddDownloadCommand>();
    ASSERT_EQ(adds.size(), 2u);
    explorer_->Handle(ldm::core::GrabberStopDownloadsCommand{id});
    Complete(adds[1].token, 101);
    const auto stops = sink_.All<ldm::core::StopCommand>();
    ASSERT_FALSE(stops.empty());
    EXPECT_EQ(stops.back().ids, std::vector<DownloadId>{DownloadId{101}});
    EXPECT_EQ(ByUrl(Items(id), "https://example.com/b.zip")->downloadId, DownloadId{101});
}

TEST_F(GrabberExplorerTest, AnAddAnsweredAfterItsProjectWasDeletedIsRemoved)
{
    Start();
    Page("https://example.com/", R"(<a href="a.zip">a</a>)");
    const GrabberProjectId id = Save(Definition("https://example.com/"));
    explorer_->Handle(ldm::core::GrabberExploreCommand{id, false, {}});
    Serve();
    explorer_->Handle(ldm::core::GrabberDownloadCommand{id});
    const auto adds = sink_.All<ldm::core::AddDownloadCommand>();
    ASSERT_EQ(adds.size(), 1u);
    explorer_->Handle(ldm::core::GrabberDeleteProjectCommand{id});
    Complete(adds[0].token, 100);
    const auto deletes = sink_.All<ldm::core::DeleteCommand>();
    ASSERT_FALSE(deletes.empty());
    EXPECT_EQ(deletes.back().ids, std::vector<DownloadId>{DownloadId{100}});
    EXPECT_FALSE(deletes.back().deleteFiles);
}

TEST_F(GrabberExplorerTest, NothingStartsAfterShutdown)
{
    Start();
    Page("https://example.com/", R"(<a href="a.html">a</a>)");
    Page("https://example.com/a.html", "x");
    const GrabberProjectId id = Save(Definition("https://example.com/"));
    explorer_->Handle(ldm::core::GrabberExploreCommand{id, false, {}});
    FetchRequest first = engine_.fetches.front();
    engine_.fetches.clear();
    explorer_->Shutdown();
    FetchFinished start;
    start.fetchId = first.fetchId;
    start.finalUrl = first.url;
    start.contentType = "text/html";
    start.body = site_.at(first.url).body;
    explorer_->OnEngineEvent(ldm::core::EngineEvent{{}, 0, start});
    EXPECT_TRUE(engine_.fetches.empty());
}

TEST_F(GrabberExplorerTest, TeeKeepsTheGrabbersOutputFromTheFrontEnd)
{
    Start();
    class Front final : public ldm::core::CoreObserver {
    public:
        void OnSnapshot(std::shared_ptr<const ldm::core::Snapshot>) override { ++snapshots; }
        void OnNotification(ldm::core::Notification) override { ++notifications; }
        int snapshots = 0;
        int notifications = 0;
    } front;
    ldm::grabber::CoreOutputTee tee(*explorer_, front);
    tee.OnSnapshot(core_);
    tee.OnNotification(ldm::core::AddResult{GrabberExplorer::kTokenBase + 7, {}, {}, {}, false});
    tee.OnNotification(ldm::core::AddResult{3, {}, {}, {}, false});
    EXPECT_EQ(front.snapshots, 1);
    EXPECT_EQ(front.notifications, 1);
}

}  // namespace
