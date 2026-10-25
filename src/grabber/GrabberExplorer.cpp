#include "grabber/GrabberExplorer.h"

#include "base/Paths.h"
#include "core/FailureText.h"
#include "core/FileNames.h"
#include "core/Matching.h"
#include "core/NetworkAddress.h"
#include "core/Scheduler.h"
#include "grabber/LocalPaths.h"
#include "grabber/ProjectDefinition.h"
#include "media/UrlReference.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace ldm::grabber {

namespace {

constexpr std::size_t kMaxProjectName = 255;
constexpr std::chrono::minutes kScheduleRecheck{1};

// Windows separators throughout, also for the '/'-separated paths below a save folder.
std::filesystem::path PathFromUtf8(const std::string& text)
{
    std::filesystem::path path(std::u8string(text.begin(), text.end()));
    path.make_preferred();
    return path;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool SameNameIgnoringCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
               return lower(x) == lower(y);
           });
}

bool IsFtp(const core::Url& url)
{
    return url.scheme == "ftp" || url.scheme == "ftps";
}

}  // namespace

GrabberExplorer::GrabberExplorer(const ExplorerPorts& ports, ExplorerConfig config) : ports_(ports), config_(config) {}

// ---------------------------------------------------------------------------------------------
// Lifecycle

void GrabberExplorer::Initialize()
{
    auto loaded = ports_.store->LoadGrabberProjects();
    if (!loaded) {
        ReportError("The Grabber projects could not be loaded: " + loaded.GetError().message);
        return;
    }
    for (core::StoredGrabberProject& row : loaded.Value()) {
        Project project;
        project.stored = std::move(row.project);
        if (auto definition = ParseDefinition(row.definitionJson)) {
            project.stored.definition = std::move(definition).Value();
        } else {
            project.damaged = definition.GetError().message;
        }
        Prepare(project);
        const model::GrabberProjectId id = project.stored.id;
        projects_.emplace(id, std::move(project));
    }
    lastScheduleCheckUnixMs_ = ports_.clock->NowUnixMs();
    PlanSchedules();
    snapshotDirty_ = true;
    Publish();
}

void GrabberExplorer::Shutdown()
{
    shutDown_ = true;
    for (auto& [id, project] : projects_) {
        SaveItems(project);
    }
}

void GrabberExplorer::Tick()
{
    const auto now = ports_.clock->Steady();
    if (now - lastSave_ >= config_.saveInterval) {
        lastSave_ = now;
        for (auto& [id, project] : projects_) {
            SaveItems(project);
        }
    }
    // Checked when a start or stop is due, and every minute in case the clock was moved.
    const std::int64_t nowUnix = ports_.clock->NowUnixMs();
    if (!shutDown_ && ports_.timeZone != nullptr &&
        ((nextScheduleEventUnixMs_ && nowUnix >= *nextScheduleEventUnixMs_) ||
         nowUnix - lastScheduleCheckUnixMs_ >= std::chrono::milliseconds(kScheduleRecheck).count() ||
         nowUnix < lastScheduleCheckUnixMs_)) {
        CheckSchedules();
    }
    if (snapshotDirty_ && now - lastSnapshot_ >= config_.snapshotInterval) {
        Publish();
    }
}

std::chrono::steady_clock::time_point GrabberExplorer::NextDeadline() const
{
    const auto now = ports_.clock->Steady();
    auto deadline = std::chrono::steady_clock::time_point::max();
    if (snapshotDirty_) {
        deadline = std::min(deadline, lastSnapshot_ + config_.snapshotInterval);
    }
    const bool unsaved = std::any_of(projects_.begin(), projects_.end(),
                                     [](const auto& entry) { return !entry.second.unsaved.empty(); });
    if (unsaved) {
        deadline = std::min(deadline, lastSave_ + config_.saveInterval);
    }
    if (ports_.timeZone != nullptr && !shutDown_) {
        std::int64_t waitMs = std::chrono::milliseconds(kScheduleRecheck).count();
        if (nextScheduleEventUnixMs_) {
            waitMs = std::clamp<std::int64_t>(*nextScheduleEventUnixMs_ - ports_.clock->NowUnixMs(), 0, waitMs);
        }
        deadline = std::min(deadline, now + std::chrono::milliseconds(waitMs));
    }
    return deadline;
}

// ---------------------------------------------------------------------------------------------
// Commands

void GrabberExplorer::Handle(core::GrabberCommand command)
{
    if (shutDown_) {
        return;
    }
    std::visit(
        [this](auto& typed) {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<T, core::GrabberSaveProjectCommand>) {
                SaveProject(std::move(typed));
            } else if constexpr (std::is_same_v<T, core::GrabberDeleteProjectCommand>) {
                DeleteProject(typed.id);
            } else if constexpr (std::is_same_v<T, core::GrabberOpenCommand>) {
                SetOpen(typed.id, typed.open);
            } else if constexpr (std::is_same_v<T, core::GrabberEditingCommand>) {
                if (Project* project = Find(typed.id)) {
                    project->editing = typed.open;
                }
            } else if constexpr (std::is_same_v<T, core::GrabberExploreCommand>) {
                Explore(typed.id, typed.keepChecks, std::move(typed.cookies), false);
                if (Project* project = Find(typed.id); project != nullptr && project->exploring && typed.thenDownload) {
                    project->downloadAfterExplore = true;
                }
            } else if constexpr (std::is_same_v<T, core::GrabberStopCommand>) {
                if (Project* project = Find(typed.id)) {
                    StopProject(*project);
                }
            } else if constexpr (std::is_same_v<T, core::GrabberCheckCommand>) {
                Check(typed);
            } else if constexpr (std::is_same_v<T, core::GrabberDownloadCommand>) {
                if (Project* project = Find(typed.id)) {
                    StartDownloads(*project, false);
                }
            } else if constexpr (std::is_same_v<T, core::GrabberStopDownloadsCommand>) {
                if (Project* project = Find(typed.id)) {
                    StopDownloads(*project);
                }
            } else if constexpr (std::is_same_v<T, core::GrabberAddToMainListCommand>) {
                if (Project* project = Find(typed.id)) {
                    AddToMainList(*project, typed.items);
                }
            } else {
                static_assert(sizeof(T) == 0, "unhandled Grabber command");
            }
        },
        command);
}

void GrabberExplorer::SaveProject(core::GrabberSaveProjectCommand command)
{
    GrabberProjectSaved saved;
    saved.token = command.token;
    const auto fail = [this, &saved](std::string error) {
        saved.error = std::move(error);
        ports_.observer->OnGrabberNotification(std::move(saved));
    };
    model::GrabberProject incoming = std::move(command.project);
    const auto first = incoming.name.find_first_not_of(" \t");
    incoming.name = first == std::string::npos ? std::string() : incoming.name.substr(first);
    incoming.name.erase(incoming.name.find_last_not_of(" \t") + 1);
    if (incoming.name.empty() || incoming.name.size() > kMaxProjectName ||
        incoming.name.find_first_of("\r\n\t") != std::string::npos) {
        fail("Enter a project name.");
        return;
    }
    for (const auto& [id, other] : projects_) {
        if (id != incoming.id && SameNameIgnoringCase(other.stored.name, incoming.name)) {
            fail("A project or template with this name already exists.");
            return;
        }
    }
    // Validated as it will be read back.
    const std::string json = DefinitionToJson(incoming.definition);
    auto definition = ParseDefinition(json);
    if (!definition) {
        fail("The project's settings are not valid: check the start page, the folder and the filters.");
        return;
    }
    incoming.definition = std::move(definition).Value();

    Project* existing = incoming.id.value != 0 ? Find(incoming.id) : nullptr;
    if (incoming.id.value != 0 && existing == nullptr) {
        fail("The project no longer exists.");
        return;
    }
    // A running project's schedule may change; what it explores may not.
    const bool sameWork = existing != nullptr && existing->stored.definition == incoming.definition &&
                          existing->stored.loginUser == incoming.loginUser && !command.loginPassword &&
                          existing->stored.isTemplate == incoming.isTemplate;
    if (existing != nullptr && (existing->exploring || existing->downloading) && !sameWork) {
        fail("Stop the project before changing it.");
        return;
    }
    incoming.loginSecret = existing != nullptr ? existing->stored.loginSecret : std::vector<std::uint8_t>{};
    if (command.loginPassword) {
        incoming.loginSecret.clear();
        if (!command.loginPassword->empty()) {
            auto secret = ports_.secrets->Protect(*command.loginPassword);
            if (!secret) {
                fail("The password could not be protected: " + secret.GetError().message);
                return;
            }
            incoming.loginSecret = std::move(secret).Value();
        }
    }
    if (incoming.loginUser.empty()) {
        incoming.loginSecret.clear();
    }
    if (existing != nullptr) {
        incoming.createdAtMs = existing->stored.createdAtMs;
        incoming.lastRunAtMs = existing->stored.lastRunAtMs;
        if (base::Status status = ports_.store->SaveGrabberProject(incoming, json); !status) {
            fail("The project could not be saved: " + status.GetError().message);
            return;
        }
        existing->stored = std::move(incoming);
        existing->damaged.reset();
        if (!sameWork) {
            Prepare(*existing);
        }
        saved.id = existing->stored.id;
    } else {
        incoming.createdAtMs = ports_.clock->NowUnixMs();
        incoming.lastRunAtMs.reset();
        auto inserted = ports_.store->InsertGrabberProject(incoming, json);
        if (!inserted) {
            fail("The project could not be saved: " + inserted.GetError().message);
            return;
        }
        incoming.id = inserted.Value();
        Project project;
        project.stored = std::move(incoming);
        Prepare(project);
        saved.id = project.stored.id;
        projects_.emplace(project.stored.id, std::move(project));
    }
    PlanSchedules();
    snapshotDirty_ = true;
    ports_.observer->OnGrabberNotification(std::move(saved));
}

void GrabberExplorer::DeleteProject(model::GrabberProjectId id)
{
    Project* project = Find(id);
    if (project == nullptr) {
        return;
    }
    StopProject(*project);
    if (base::Status status = ports_.store->DeleteGrabberProject(id); !status) {
        ReportError("The project could not be deleted: " + status.GetError().message);
        return;
    }
    AbandonPendingAdds(*project);
    RemoveHiddenDownloads(id);
    std::erase_if(fetches_, [id](const auto& entry) { return entry.second.project == id; });
    projects_.erase(id);
    PlanSchedules();
    snapshotDirty_ = true;
}

void GrabberExplorer::AbandonPendingAdds(Project& project)
{
    for (const auto& [token, itemId] : project.adding) {
        abandonedAdds_.insert(token);
    }
    project.adding.clear();
}

bool GrabberExplorer::AddPending(const Project& project, std::int64_t itemId)
{
    return std::any_of(project.adding.begin(), project.adding.end(),
                       [itemId](const auto& entry) { return entry.second == itemId; });
}

void GrabberExplorer::RemoveHiddenDownloads(model::GrabberProjectId id)
{
    // Downloads no item refers to any more would stay hidden for good. Files on disk stay.
    core::DeleteCommand removal;
    if (core_) {
        for (const auto& view : core_->downloads) {
            if (view->download.projectId == id && !view->download.listed) {
                removal.ids.push_back(view->download.id);
            }
        }
    }
    if (!removal.ids.empty()) {
        ports_.downloads->Post(std::move(removal));
    }
}

void GrabberExplorer::SetOpen(model::GrabberProjectId id, bool open)
{
    Project* project = Find(id);
    if (project == nullptr || project->open == open) {
        return;
    }
    project->open = open;
    if (open) {
        if (!LoadItems(*project)) {
            project->open = false;
        }
    } else {
        if (project->stored.definition.addToMainListOnClose) {
            AddToMainList(*project, {});
        }
        UnloadItemsIfIdle(*project);
    }
    snapshotDirty_ = true;
}

void GrabberExplorer::Explore(model::GrabberProjectId id, bool keepChecks, std::vector<model::Cookie> cookies,
                              bool scheduled)
{
    Project* found = Find(id);
    if (found == nullptr || found->stored.isTemplate || found->exploring) {
        return;
    }
    Project& project = *found;
    if (project.damaged || !project.start) {
        ReportError("The project \"" + project.stored.name + "\" cannot be explored: its settings are damaged.");
        return;
    }
    if (!keepChecks && project.downloading) {
        StopDownloads(project);  // before loading the list: stopping may unload it
    }
    if (!LoadItems(project)) {
        return;
    }
    if (!keepChecks) {
        if (base::Status status = ports_.store->ClearGrabberItems(project.stored.id); !status) {
            ReportError("The project's list could not be cleared: " + status.GetError().message);
            return;
        }
        AbandonPendingAdds(project);
        RemoveHiddenDownloads(project.stored.id);
        project.items.clear();
        project.byAddress.clear();
        project.unsaved.clear();
    }
    for (Item& item : project.items) {
        item.readThisRun = false;
        item.otherSitesDepth = 0;
    }
    project.exploring = true;
    ++project.run;
    project.fetching = 0;  // answers of an earlier run no longer count
    project.startPageFailure.reset();
    project.scheduledRun = scheduled;
    project.pagesExplored = 0;
    project.limitReached = false;
    project.pageQueue.clear();
    project.sizeQueue.clear();
    std::erase_if(cookies, [](const model::Cookie& cookie) { return !model::IsValidCookie(cookie); });
    project.cookieLines = model::CookieLines(model::CookiesToText(cookies));
    project.cookies = std::move(cookies);

    const std::string startKey = project.stored.definition.files.hideDuplicates ? NormalizeAddress(*project.start)
                                                                                 : project.stored.definition.startPage;
    std::int64_t startId = 0;
    if (const auto existing = project.byAddress.find(startKey); existing != project.byAddress.end()) {
        startId = existing->second;
    } else {
        model::GrabberItem start;
        start.url = project.stored.definition.startPage;
        start.kind = model::GrabberItemKind::Page;
        start.depth = 0;
        startId = NewItem(project, std::move(start), startKey).data.id;
    }
    Item* startItem = FindItem(project, startId);
    startItem->readThisRun = true;
    project.pageQueue.push_back(startId);
    PumpFetches(project);
    snapshotDirty_ = true;
}

void GrabberExplorer::StopProject(Project& project)
{
    if (project.exploring) {
        project.exploring = false;
        project.downloadAfterExplore = false;
        project.pageQueue.clear();
        project.sizeQueue.clear();
        // Answers of fetches still running are dropped when they come (another run); lookups not
        // answered yet are not fetched at all.
        ++project.run;
        project.fetching = 0;
        for (auto it = waitingForProxy_.begin(); it != waitingForProxy_.end();) {
            const auto fetch = fetches_.find(it->first);
            if (fetch != fetches_.end() && fetch->second.project == project.stored.id) {
                fetches_.erase(fetch);
                it = waitingForProxy_.erase(it);
            } else {
                ++it;
            }
        }
        project.cookieLines.clear();
        project.cookies.clear();
        WritePendingPages(project);  // what was read is kept
        SaveProjectRow(project);
    }
    StopDownloads(project);
    snapshotDirty_ = true;
}

void GrabberExplorer::Check(const core::GrabberCheckCommand& command)
{
    Project* project = Find(command.id);
    if (project == nullptr || !project->itemsLoaded) {
        return;
    }
    const auto apply = [&](Item& item) {
        if (item.data.kind != model::GrabberItemKind::File || item.data.checked == command.checked) {
            return;
        }
        item.data.checked = command.checked;
        Changed(*project, item);
    };
    if (command.all) {
        for (Item& item : project->items) {
            if (item.data.state != model::GrabberItemState::Filtered) {
                apply(item);
            }
        }
        return;
    }
    for (const std::int64_t id : command.items) {
        if (Item* item = FindItem(*project, id)) {
            apply(*item);
        }
    }
}

void GrabberExplorer::StartDownloads(Project& project, bool scheduled)
{
    if (project.stored.isTemplate || project.damaged || !LoadItems(project)) {
        return;
    }
    project.scheduledRun = project.scheduledRun || scheduled;
    for (Item& item : project.items) {
        const bool wanted = item.data.kind == model::GrabberItemKind::File && item.data.checked &&
                            item.data.state != model::GrabberItemState::Done &&
                            item.data.state != model::GrabberItemState::Downloading;
        if (wanted && std::find(project.downloadQueue.begin(), project.downloadQueue.end(), item.data.id) ==
                          project.downloadQueue.end()) {
            item.failures = 0;
            project.downloadQueue.push_back(item.data.id);
        }
    }
    project.downloading = true;
    PumpDownloads(project);
    FinishDownloadsIfDone(project);
    snapshotDirty_ = true;
}

void GrabberExplorer::StopDownloads(Project& project)
{
    if (!project.downloading) {
        return;
    }
    project.downloading = false;
    project.downloadQueue.clear();
    core::StopCommand stop;
    for (const auto& [downloadId, itemId] : project.active) {
        stop.ids.push_back(downloadId);
        if (Item* item = FindItem(project, itemId)) {
            item->data.state = model::GrabberItemState::Found;
            Changed(project, *item);
        }
    }
    // Adds not answered yet stay in `adding`: their answer stops the download, or adopts it when
    // downloading started again meanwhile.
    for (const auto& [token, itemId] : project.adding) {
        if (Item* item = FindItem(project, itemId)) {
            item->data.state = model::GrabberItemState::Found;
            Changed(project, *item);
        }
    }
    project.active.clear();
    project.seenRunning.clear();
    if (!stop.ids.empty()) {
        ports_.downloads->Post(std::move(stop));
    }
    UnloadItemsIfIdle(project);
    snapshotDirty_ = true;
}

void GrabberExplorer::AddToMainList(Project& project, const std::vector<std::int64_t>& items)
{
    if (!project.itemsLoaded && !LoadItems(project)) {
        return;
    }
    core::SetListedCommand listed;
    const auto add = [&](const Item& item) {
        if (item.data.downloadId) {
            listed.ids.push_back(*item.data.downloadId);
        }
    };
    if (items.empty()) {
        for (const Item& item : project.items) {
            if (item.data.checked) {
                add(item);
            }
        }
    } else {
        for (const std::int64_t id : items) {
            if (const Item* item = FindItem(project, id)) {
                add(*item);
            }
        }
    }
    if (!listed.ids.empty()) {
        ports_.downloads->Post(std::move(listed));
    }
}

// ---------------------------------------------------------------------------------------------
// Exploring

void GrabberExplorer::PumpFetches(Project& project)
{
    const int limit = std::clamp(project.stored.definition.exploreConcurrency, 1, model::kMaxGrabberConcurrency);
    while (project.exploring && !shutDown_ && project.fetching < limit) {
        if (!project.pageQueue.empty()) {
            const std::int64_t id = project.pageQueue.front();
            project.pageQueue.pop_front();
            StartFetch(project, id, false);
        } else if (!project.sizeQueue.empty()) {
            const std::int64_t id = project.sizeQueue.front();
            project.sizeQueue.pop_front();
            StartFetch(project, id, true);
        } else {
            break;
        }
    }
    FinishExploreIfDone(project);
}

void GrabberExplorer::StartFetch(Project& project, std::int64_t itemId, bool head)
{
    Item* item = FindItem(project, itemId);
    if (item == nullptr) {
        return;
    }
    const auto url = core::ParseUrl(item->data.url);
    if (!url || IsFtp(*url)) {
        // Only http and https pages are read; an FTP file keeps an unknown size.
        if (head) {
            item->data.state = model::GrabberItemState::Found;
            Changed(project, *item);
        }
        return;
    }
    core::FetchRequest request;
    request.fetchId = nextFetchId_--;
    request.url = item->data.url;
    if (item->data.parentId) {
        if (const Item* parent = FindItem(project, *item->data.parentId)) {
            request.referer = parent->data.url;
        }
    }
    request.userAgent = core_ ? core_->settings.userAgent : std::string();
    request.cookieLines = project.cookieLines;
    request.maxBytes = config_.maxPageBytes;
    request.refusePrivateAddresses = !project.startIsPrivate;
    request.credentials = CredentialsFor(project, *url);
    request.headOnly = head;
    if (!head) {
        request.bodyTypes = {"text/html", "application/xhtml+xml"};
    }
    fetches_.emplace(request.fetchId, Fetch{project.stored.id, itemId, head, project.run});
    ++project.fetching;
    const core::ProxyLookup lookup = ports_.proxy->Resolve(model::DownloadId{request.fetchId}, 0, request.url);
    if (lookup.pending) {
        const std::int64_t id = request.fetchId;
        waitingForProxy_.emplace(id, std::move(request));
        return;
    }
    request.proxy = lookup.proxy;
    ports_.engine->Fetch(std::move(request));
}

void GrabberExplorer::OnEngineEvent(core::EngineEvent event)
{
    if (shutDown_) {
        return;  // LDM is ending: nothing new starts, and the list was saved
    }
    if (auto* fetched = std::get_if<core::FetchFinished>(&event.payload)) {
        const auto it = fetches_.find(fetched->fetchId);
        if (it == fetches_.end()) {
            return;
        }
        const Fetch fetch = it->second;
        fetches_.erase(it);
        OnFetched(fetch, *fetched);
        return;
    }
    if (const auto* resolved = std::get_if<core::ProxyResolved>(&event.payload)) {
        const auto waiting = waitingForProxy_.find(event.id.value);
        if (waiting == waitingForProxy_.end()) {
            return;
        }
        core::FetchRequest request = std::move(waiting->second);
        waitingForProxy_.erase(waiting);
        request.proxy = resolved->proxy;
        ports_.engine->Fetch(std::move(request));
    }
}

void GrabberExplorer::OnFetched(const Fetch& fetch, const core::FetchFinished& finished)
{
    Project* project = Find(fetch.project);
    if (project == nullptr || fetch.run != project->run) {
        return;  // of a run that was stopped or replaced
    }
    --project->fetching;
    Item* item = FindItem(*project, fetch.item);
    if (!project->exploring || item == nullptr) {
        FinishExploreIfDone(*project);
        return;
    }
    if (fetch.head) {
        if (!finished.failure && finished.contentType) {
            item->data.contentType = *finished.contentType;
        }
        // A server that does not tell the size leaves the file listed, for the user to decide.
        ClassifyFile(*project, *item, finished.failure ? std::nullopt : finished.contentLength);
    } else if (finished.failure) {
        item->data.state = model::GrabberItemState::Failed;
        Changed(*project, *item);
        if (!item->data.parentId) {
            project->startPageFailure = std::string(core::DescribeFailureReason(finished.failure->reason));
        }
    } else {
        OnPageRead(*project, *item, finished);
    }
    PumpFetches(*project);
    snapshotDirty_ = true;
}

void GrabberExplorer::OnPageRead(Project& project, Item& item, const core::FetchFinished& finished)
{
    if (finished.contentType) {
        item.data.contentType = *finished.contentType;
    }
    if (finished.bodySkipped || !finished.contentType || !IsPageType(*finished.contentType)) {
        // Not a page after all: a file, judged by the file filters.
        item.data.kind = model::GrabberItemKind::File;
        ClassifyFile(project, item, finished.contentLength);
        return;
    }
    item.data.state = model::GrabberItemState::Done;
    item.data.size = static_cast<std::int64_t>(finished.body.size());
    Changed(project, item);
    if (!item.data.parentId && !finished.finalUrl.empty()) {
        // A start page that redirects to another host of its domain ("example.com" to
        // "www.example.com") makes that host the site: every link on it is there. Another domain
        // is not adopted, so the project's login never follows a redirect off the site.
        const auto landed = core::ParseUrl(finished.finalUrl);
        if (landed && landed->host != project.site.host && MainDomainOf(landed->host) == project.site.mainDomain) {
            project.site = BaseSiteOf(*landed);
        }
    }
    if (++project.pagesExplored >= config_.maxPagesExplored) {
        project.limitReached = true;
        project.pageQueue.clear();
    }
    const PageLinks links = ScanLinks(finished.body, config_.maxLinksPerPage);
    if (links.truncated) {
        project.limitReached = true;
    }
    std::string base = finished.finalUrl.empty() ? item.data.url : finished.finalUrl;
    if (links.baseHref) {
        if (auto resolved = media::ResolveLinkUrl(base, *links.baseHref)) {
            base = std::move(*resolved);
        }
    }
    const std::int64_t itemId = item.data.id;
    for (const HtmlLink& link : links.links) {
        // AddLink may grow the item vector, so the page is looked up again each time.
        const Item* page = FindItem(project, itemId);
        AddLink(project, *page, link, base);
    }
    if (project.stored.definition.savePages) {
        // Written once exploring ends, when every link's file is decided (some wait for their
        // size); a large site past the budget gets the rest written at once, with what is known.
        PendingPage pending{itemId, finished.body, base, links.links, links.baseHrefRange};
        if (project.pendingBytes + pending.body.size() > config_.maxPendingPageBytes) {
            WritePage(project, pending);
        } else {
            project.pendingBytes += pending.body.size();
            project.pendingPages.push_back(std::move(pending));
        }
    }
}

void GrabberExplorer::AddLink(Project& project, const Item& parent, const HtmlLink& link, const std::string& base)
{
    const auto resolved = media::ResolveLinkUrl(base, link.address);
    if (!resolved) {
        return;
    }
    const auto url = core::ParseUrl(*resolved);
    if (!url || url->hasUserInfo) {
        return;
    }
    // A crawl that started on the internet is never steered into the home network.
    if (!project.startIsPrivate && core::IsPrivateHost(url->host)) {
        return;
    }
    const model::GrabberProjectDefinition& definition = project.stored.definition;
    const std::string key = definition.files.hideDuplicates ? NormalizeAddress(*url) : *resolved;
    const bool onBase = OnBaseSite(project.site, *url, definition.explore.mainDomainSites);
    const PageDepth depth = DepthOfLink(PageDepth{parent.data.depth, parent.otherSitesDepth}, onBase);

    if (const auto existing = project.byAddress.find(key); existing != project.byAddress.end()) {
        Item* item = FindItem(project, existing->second);
        if (item == nullptr) {
            return;
        }
        if (item->data.linkText.empty() && !link.text.empty()) {
            item->data.linkText = link.text;
            Changed(project, *item);
        }
        // Update all: a page found before is read again in this run when the filters allow.
        if (item->data.kind == model::GrabberItemKind::Page && !item->readThisRun && project.exploring &&
            !project.limitReached && ShouldExplore(definition.explore, project.site, *url, depth)) {
            item->readThisRun = true;
            item->data.depth = std::min(item->data.depth, depth.depth);
            item->otherSitesDepth = depth.otherSitesDepth;
            project.pageQueue.push_back(item->data.id);
        }
        return;
    }
    if (project.items.size() >= config_.maxItems) {
        project.limitReached = true;
        return;
    }

    bool page = false;
    switch (link.kind) {
    case LinkKind::Embedded:
        page = false;
        break;
    case LinkKind::Frame:
    case LinkKind::Refresh:
    case LinkKind::Form:
        page = true;
        break;
    case LinkKind::Anchor:
        page = LooksLikePage(*url);
        break;
    }
    if (IsFtp(*url)) {
        page = false;
    }

    model::GrabberItem data;
    data.url = *resolved;
    data.kind = page ? model::GrabberItemKind::Page : model::GrabberItemKind::File;
    data.parentId = parent.data.id;
    data.depth = depth.depth;
    data.linkText = link.text;
    Item& item = NewItem(project, std::move(data), key);
    item.otherSitesDepth = depth.otherSitesDepth;
    if (page) {
        if (!project.limitReached && ShouldExplore(definition.explore, project.site, *url, depth)) {
            item.readThisRun = true;
            project.pageQueue.push_back(item.data.id);
        }
        return;
    }
    ClassifyFile(project, item, std::nullopt);
}

void GrabberExplorer::ClassifyFile(Project& project, Item& item, std::optional<std::int64_t> size)
{
    const model::GrabberProjectDefinition& definition = project.stored.definition;
    if (size) {
        item.data.size = size;
    }
    const auto url = core::ParseUrl(item.data.url);
    const FileMatch match = url ? MatchFile(definition.files, project.site, definition.explore.mainDomainSites, *url,
                                            item.data.size)
                                : FileMatch::Filtered;
    if (match == FileMatch::NeedsSize) {
        // Listed unchecked: asked once with a HEAD request, or, when the server cannot tell the
        // size, left for the user to decide.
        item.data.state = model::GrabberItemState::Found;
        item.data.checked = false;
        Changed(project, item);
        if (!size && !item.sizeAsked && url && !IsFtp(*url) && project.exploring) {
            item.sizeAsked = true;
            project.sizeQueue.push_back(item.data.id);
        }
        return;
    }
    if (item.data.state == model::GrabberItemState::Done || item.data.state == model::GrabberItemState::Downloading) {
        Changed(project, item);
        return;
    }
    item.data.state = match == FileMatch::Match ? model::GrabberItemState::Found : model::GrabberItemState::Filtered;
    item.data.checked = match == FileMatch::Match;
    Changed(project, item);
    if (item.data.checked && definition.files.downloadImmediately) {
        project.downloadQueue.push_back(item.data.id);
        project.downloading = true;
        PumpDownloads(project);
    }
}

void GrabberExplorer::WritePendingPages(Project& project)
{
    for (const PendingPage& page : project.pendingPages) {
        WritePage(project, page);
    }
    project.pendingPages.clear();
    project.pendingBytes = 0;
}

void GrabberExplorer::WritePage(Project& project, const PendingPage& page)
{
    const Item* item = FindItem(project, page.itemId);
    const auto root = SaveRoot(project);
    const auto pageUrl = item != nullptr ? core::ParseUrl(item->data.url) : std::nullopt;
    if (!root || !pageUrl) {
        return;
    }
    const std::string& base = page.base;
    const model::GrabberProjectDefinition& definition = project.stored.definition;
    const std::string pagePath = LocalRelativePath(*pageUrl, true, definition.keepSiteFolders);
    std::string body = page.body;
    if (definition.convertLinks) {
        // Links to what this project saves point at the saved copies; the rest stay on the web.
        std::vector<LinkReplacement> replacements;
        replacements.reserve(page.links.size());
        for (const HtmlLink& link : page.links) {
            const auto resolved = media::ResolveLinkUrl(base, link.address);
            if (!resolved) {
                continue;
            }
            const auto url = core::ParseUrl(*resolved);
            if (!url) {
                continue;
            }
            const std::string key = definition.files.hideDuplicates ? NormalizeAddress(*url) : *resolved;
            const auto found = project.byAddress.find(key);
            const Item* target = found == project.byAddress.end() ? nullptr : FindItem(project, found->second);
            const bool savedHere = target != nullptr &&
                                   ((target->data.kind == model::GrabberItemKind::Page && target->readThisRun) ||
                                    (target->data.kind == model::GrabberItemKind::File && target->data.checked));
            std::string reference;
            if (savedHere) {
                const bool targetIsPage = target->data.kind == model::GrabberItemKind::Page;
                reference = RelativeReference(pagePath, LocalRelativePath(*url, targetIsPage, definition.keepSiteFolders));
            } else {
                reference = AttributeSafeAddress(*resolved);
            }
            replacements.push_back(LinkReplacement{link.begin, link.end, std::move(reference)});
        }
        if (page.baseHrefRange) {
            // The converted links are relative to the saved copy or absolute; a <base> naming the
            // website would send the relative ones back there, so it names the saved page itself.
            replacements.push_back(LinkReplacement{page.baseHrefRange->begin, page.baseHrefRange->end,
                                                   RelativeReference(pagePath, pagePath)});
        }
        body = RewriteLinks(page.body, std::move(replacements));
    }
    const std::filesystem::path target = *root / PathFromUtf8(pagePath);
    if (base::Status created = ports_.fileSystem->CreateDirectories(target.parent_path()); !created) {
        ReportError("A page could not be saved: " + created.GetError().message);
        return;
    }
    base::Status written = ports_.fileSystem->WriteFile(target, body, definition.overwrite);
    if (!written && written.GetError().code != base::ErrorCode::AlreadyExists) {
        ReportError("A page could not be saved: " + written.GetError().message);
        return;
    }
    if (Item* saved = FindItem(project, page.itemId)) {
        saved->data.localPath = PathToUtf8(target);
        Changed(project, *saved);
    }
}

void GrabberExplorer::FinishExploreIfDone(Project& project)
{
    if (!project.exploring || project.fetching > 0 || !project.pageQueue.empty() || !project.sizeQueue.empty()) {
        return;
    }
    project.exploring = false;
    WritePendingPages(project);
    project.cookieLines.clear();
    project.cookies.clear();
    project.stored.lastRunAtMs = ports_.clock->NowUnixMs();
    SaveProjectRow(project);
    SaveItems(project);
    ports_.observer->OnGrabberNotification(
        GrabberRunFinished{project.stored.id, project.stored.name, true, project.scheduledRun});
    if (project.downloadAfterExplore) {
        project.downloadAfterExplore = false;
        StartDownloads(project, project.scheduledRun);
    } else if (!project.downloading) {
        project.scheduledRun = false;
    }
    // Downloads that ran while exploring ("download immediately") may all be done already.
    FinishDownloadsIfDone(project);
    UnloadItemsIfIdle(project);
    snapshotDirty_ = true;
}

// ---------------------------------------------------------------------------------------------
// Downloading

void GrabberExplorer::PumpDownloads(Project& project)
{
    const int limit = std::clamp(project.stored.definition.downloadConcurrency, 1, model::kMaxGrabberConcurrency);
    while (project.downloading && static_cast<int>(project.active.size() + project.adding.size()) < limit &&
           !project.downloadQueue.empty()) {
        const std::int64_t id = project.downloadQueue.front();
        project.downloadQueue.pop_front();
        Item* item = FindItem(project, id);
        // An item whose earlier add is still on its way is taken over by that add's answer.
        if (item == nullptr || !item->data.checked || item->data.state == model::GrabberItemState::Done ||
            item->data.state == model::GrabberItemState::Downloading || AddPending(project, id)) {
            continue;
        }
        StartDownload(project, *item);
    }
}

void GrabberExplorer::StartDownload(Project& project, Item& item)
{
    // A download made for this item before is resumed rather than added again.
    if (item.data.downloadId) {
        if (const core::DownloadView* view = FindDownload(*item.data.downloadId)) {
            if (view->download.status == model::DownloadStatus::Completed) {
                item.data.state = model::GrabberItemState::Done;
                item.data.localPath = PathToUtf8(PathFromUtf8(view->download.saveDir) / PathFromUtf8(view->download.fileName));
                item.data.size = view->download.totalSize;
                Changed(project, item);
                return;
            }
            item.data.state = model::GrabberItemState::Downloading;
            Changed(project, item);
            project.active.emplace(*item.data.downloadId, item.data.id);
            ports_.downloads->Post(core::ResumeCommand{{*item.data.downloadId}});
            return;
        }
        item.data.downloadId.reset();
    }
    const auto url = core::ParseUrl(item.data.url);
    if (!url) {
        item.data.state = model::GrabberItemState::Failed;
        Changed(project, item);
        return;
    }
    const model::GrabberProjectDefinition& definition = project.stored.definition;
    core::AddDownloadCommand add;
    add.token = nextToken_++;
    add.url = item.data.url;
    add.projectId = project.stored.id;
    add.listed = false;
    add.mode = core::AddMode::StartNow;
    add.duplicatePolicy = model::DuplicatePolicy::AddNumbered;
    add.existingFilePolicy = definition.overwrite ? core::ExistingFilePolicy::Replace : core::ExistingFilePolicy::AutoRename;
    if (item.data.parentId) {
        if (const Item* parent = FindItem(project, *item.data.parentId)) {
            add.referer = parent->data.url;
            add.parentPage = parent->data.url;
        }
    }
    if (definition.linkTextAsDescription) {
        add.description = item.data.linkText;
    }
    if (!project.cookies.empty()) {
        add.cookies = project.cookies;
    }
    if (const auto credentials = CredentialsFor(project, *url)) {
        add.loginUser = credentials->user;
        add.loginPassword = credentials->password;
    }
    if (definition.saveMode == model::GrabberSaveMode::OneCategory) {
        add.categoryId = definition.category;
    }
    if (const auto root = SaveRoot(project)) {
        if (Offline(project)) {
            // Saved where the pages' links expect it.
            const std::filesystem::path planned = *root / PathFromUtf8(LocalRelativePath(*url, false, definition.keepSiteFolders));
            add.saveDir = PathToUtf8(planned.parent_path());
            add.fileName = PathToUtf8(planned.filename());
        } else if (definition.keepSiteFolders) {
            const std::filesystem::path planned = *root / PathFromUtf8(LocalRelativePath(*url, false, true));
            add.saveDir = PathToUtf8(planned.parent_path());
        } else if (definition.saveMode == model::GrabberSaveMode::OneFolder) {
            add.saveDir = PathToUtf8(*root);
        }
    } else if (definition.keepSiteFolders && core_ && !core_->categories.empty()) {
        // By category: the category the main list would choose, with the site's folders below it.
        const std::string name = core::FileNameFromUrlPath(url->path);
        const auto categoryId = core::MatchCategory(core_->categories, name, *url);
        const auto category = std::find_if(core_->categories.begin(), core_->categories.end(), [&](const model::Category& c) {
            return categoryId ? c.id == *categoryId : (c.builtin && c.extensions.empty());
        });
        if (category != core_->categories.end() && !category->folder.empty()) {
            const std::filesystem::path planned =
                PathFromUtf8(category->folder) / PathFromUtf8(LocalRelativePath(*url, false, true));
            add.saveDir = PathToUtf8(planned.parent_path());
            add.categoryId = category->id;
        }
    }
    item.data.state = model::GrabberItemState::Downloading;
    Changed(project, item);
    project.adding.emplace(add.token, item.data.id);
    ports_.downloads->Post(std::move(add));
}

bool GrabberExplorer::TakeNotification(const core::Notification& notification)
{
    if (const auto* added = std::get_if<core::AddResult>(&notification)) {
        if (added->token < kTokenBase) {
            return false;
        }
        if (abandonedAdds_.erase(added->token) > 0) {
            // Its item is gone: the download would stay hidden for good.
            if (added->id) {
                ports_.downloads->Post(core::DeleteCommand{{*added->id}, false});
            }
            return true;
        }
        for (auto& [projectId, project] : projects_) {
            const auto it = project.adding.find(added->token);
            if (it == project.adding.end()) {
                continue;
            }
            const std::int64_t itemId = it->second;
            project.adding.erase(it);
            OnAddAnswered(project, itemId, added->id);
            break;
        }
        return true;
    }
    if (const auto* completed = std::get_if<core::DownloadCompleted>(&notification)) {
        for (auto& [projectId, project] : projects_) {
            const auto it = project.active.find(completed->id);
            if (it != project.active.end()) {
                const std::int64_t itemId = it->second;
                project.active.erase(it);
                project.seenRunning.erase(completed->id);
                OnDownloadEnded(project, itemId, true, completed->finalPath, completed->size);
                break;
            }
        }
        return !completed->inMainList;
    }
    if (const auto* failed = std::get_if<core::DownloadFailed>(&notification)) {
        for (auto& [projectId, project] : projects_) {
            const auto it = project.active.find(failed->id);
            if (it != project.active.end()) {
                const std::int64_t itemId = it->second;
                project.active.erase(it);
                project.seenRunning.erase(failed->id);
                OnDownloadEnded(project, itemId, false, {}, std::nullopt);
                break;
            }
        }
        return !failed->inMainList;
    }
    return false;
}

void GrabberExplorer::OnAddAnswered(Project& project, std::int64_t itemId, std::optional<model::DownloadId> downloadId)
{
    Item* item = FindItem(project, itemId);
    if (!downloadId) {
        if (project.downloading && item != nullptr) {
            OnDownloadEnded(project, itemId, false, {}, std::nullopt);
        } else if (item != nullptr && item->data.state == model::GrabberItemState::Downloading) {
            item->data.state = model::GrabberItemState::Found;
            Changed(project, *item);
        }
        UnloadItemsIfIdle(project);
        return;
    }
    if (item != nullptr) {
        item->data.downloadId = downloadId;
        // Downloading again since the add was sent (Stop, then Download): this download serves.
        if (project.downloading && item->data.checked && item->data.state != model::GrabberItemState::Done) {
            item->data.state = model::GrabberItemState::Downloading;
            Changed(project, *item);
            std::erase(project.downloadQueue, itemId);
            project.active.emplace(*downloadId, itemId);
            return;
        }
        if (item->data.state == model::GrabberItemState::Downloading) {
            item->data.state = model::GrabberItemState::Found;
        }
        Changed(project, *item);
    }
    // Stopped while the add was on its way; a later Download resumes it through downloadId.
    ports_.downloads->Post(core::StopCommand{{*downloadId}});
    PumpDownloads(project);
    FinishDownloadsIfDone(project);
    UnloadItemsIfIdle(project);
    snapshotDirty_ = true;
}

void GrabberExplorer::OnDownloadEnded(Project& project, std::int64_t itemId, bool completed,
                                      const std::string& finalPath, std::optional<std::int64_t> size)
{
    Item* item = FindItem(project, itemId);
    if (item != nullptr) {
        if (completed) {
            item->data.state = model::GrabberItemState::Done;
            item->data.localPath = finalPath;
            if (size) {
                item->data.size = size;
            }
        } else if (++item->failures < config_.maxFileAttempts && project.downloading) {
            item->data.state = model::GrabberItemState::Found;
            project.downloadQueue.push_back(itemId);
        } else {
            item->data.state = model::GrabberItemState::Failed;
        }
        Changed(project, *item);
    }
    PumpDownloads(project);
    FinishDownloadsIfDone(project);
    snapshotDirty_ = true;
}

void GrabberExplorer::FinishDownloadsIfDone(Project& project)
{
    if (!project.downloading || !project.active.empty() || !project.adding.empty() || !project.downloadQueue.empty() ||
        (project.exploring && project.stored.definition.files.downloadImmediately)) {
        return;
    }
    project.downloading = false;
    project.seenRunning.clear();
    SaveItems(project);
    ports_.observer->OnGrabberNotification(
        GrabberRunFinished{project.stored.id, project.stored.name, false, project.scheduledRun});
    if (!project.exploring) {
        project.scheduledRun = false;
    }
    UnloadItemsIfIdle(project);
    snapshotDirty_ = true;
}

const core::DownloadView* GrabberExplorer::FindDownload(model::DownloadId id) const
{
    if (!core_) {
        return nullptr;
    }
    for (const auto& view : core_->downloads) {
        if (view->download.id == id) {
            return view.get();
        }
    }
    return nullptr;
}

void GrabberExplorer::OnCoreSnapshot(std::shared_ptr<const core::Snapshot> snapshot)
{
    core_ = std::move(snapshot);
    // A download of a project that someone else stopped (Stop All) or deleted no longer runs for it.
    for (auto& [projectId, project] : projects_) {
        if (project.active.empty()) {
            continue;
        }
        std::vector<model::DownloadId> gone;
        for (const auto& [downloadId, itemId] : project.active) {
            const core::DownloadView* view = FindDownload(downloadId);
            if (view == nullptr) {
                gone.push_back(downloadId);
                continue;
            }
            const model::DownloadStatus status = view->download.status;
            if (status == model::DownloadStatus::Paused) {
                // Only once it ran since it was started: a resume may not be handled yet.
                if (project.seenRunning.contains(downloadId)) {
                    gone.push_back(downloadId);
                }
            } else if (status != model::DownloadStatus::Completed && status != model::DownloadStatus::Failed) {
                project.seenRunning.insert(downloadId);
            }
        }
        for (const model::DownloadId downloadId : gone) {
            const std::int64_t itemId = project.active[downloadId];
            project.active.erase(downloadId);
            project.seenRunning.erase(downloadId);
            if (Item* item = FindItem(project, itemId)) {
                item->data.state = model::GrabberItemState::Found;
                if (!FindDownload(downloadId)) {
                    item->data.downloadId.reset();
                }
                Changed(project, *item);
            }
        }
        if (!gone.empty()) {
            PumpDownloads(project);
            FinishDownloadsIfDone(project);
            snapshotDirty_ = true;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Where things are saved

bool GrabberExplorer::Offline(const Project& project) const
{
    return project.stored.definition.savePages || project.stored.definition.convertLinks;
}

std::optional<std::filesystem::path> GrabberExplorer::SaveRoot(const Project& project) const
{
    const model::GrabberProjectDefinition& definition = project.stored.definition;
    switch (definition.saveMode) {
    case model::GrabberSaveMode::OneFolder:
        return PathFromUtf8(definition.folder);
    case model::GrabberSaveMode::OneCategory:
    case model::GrabberSaveMode::ByCategory: {
        if (!core_) {
            return std::nullopt;
        }
        // By category, an offline copy still needs one folder for its relative links: General's.
        if (definition.saveMode == model::GrabberSaveMode::ByCategory && !Offline(project)) {
            return std::nullopt;
        }
        const auto category = std::find_if(core_->categories.begin(), core_->categories.end(), [&](const model::Category& c) {
            return definition.saveMode == model::GrabberSaveMode::OneCategory
                       ? definition.category && c.id == *definition.category
                       : (c.builtin && c.extensions.empty());
        });
        if (category == core_->categories.end() || category->folder.empty()) {
            return std::nullopt;
        }
        return PathFromUtf8(category->folder);
    }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// Schedules

void GrabberExplorer::CheckSchedules()
{
    const std::int64_t now = ports_.clock->NowUnixMs();
    if (now < lastScheduleCheckUnixMs_) {
        lastScheduleCheckUnixMs_ = now;
        PlanSchedules();
        return;
    }
    const std::int64_t after = std::max(lastScheduleCheckUnixMs_, now - core::kScheduleGraceMs);
    lastScheduleCheckUnixMs_ = now;
    std::vector<model::GrabberProjectId> ids;
    for (const auto& [id, project] : projects_) {
        ids.push_back(id);
    }
    for (const model::GrabberProjectId id : ids) {
        Project* project = Find(id);
        if (project == nullptr || project->stored.isTemplate || project->damaged) {
            continue;
        }
        const model::Schedule& schedule = project->stored.schedule;
        auto start = core::NextScheduledStart(schedule, after, *ports_.timeZone);
        auto stop = core::NextScheduledStop(schedule, after, *ports_.timeZone);
        if (start && (*start > now || (schedule.mode == model::ScheduleMode::Once && schedule.lastStartedAtMs &&
                                       *schedule.lastStartedAtMs >= *start))) {
            start.reset();
        }
        if (stop && *stop > now) {
            stop.reset();
        }
        // As for queues: a stop after a start in the same check leaves the project stopped; a stop at
        // the moment of a start ends the previous run first.
        if (start && stop && *stop > *start) {
            if (project->scheduledRun) {
                StopProject(*project);
            }
            continue;
        }
        if (stop && project->scheduledRun) {
            StopProject(*project);
        }
        if (!start) {
            continue;
        }
        // A project that runs, or whose window or wizard is open, is not started (checklist G-08).
        if (project->exploring || project->downloading || project->open || project->editing) {
            continue;
        }
        project->stored.schedule.lastStartedAtMs = now;
        SaveProjectRow(*project);
        switch (project->stored.scheduleMode) {
        case model::GrabberScheduleMode::Explore:
            Explore(id, true, {}, true);
            break;
        case model::GrabberScheduleMode::ExploreDownload:
            Explore(id, true, {}, true);
            if (Project* running = Find(id); running != nullptr && running->exploring) {
                running->downloadAfterExplore = true;
            }
            break;
        case model::GrabberScheduleMode::DownloadChecked:
            StartDownloads(*project, true);
            break;
        }
        snapshotDirty_ = true;
    }
    PlanSchedules();
}

void GrabberExplorer::PlanSchedules()
{
    nextScheduleEventUnixMs_.reset();
    if (ports_.timeZone == nullptr) {
        return;
    }
    std::optional<std::int64_t> nextStart;
    for (const auto& [id, project] : projects_) {
        if (project.stored.isTemplate || project.damaged) {
            continue;
        }
        const auto start = core::NextScheduledStart(project.stored.schedule, lastScheduleCheckUnixMs_, *ports_.timeZone);
        const auto stop = core::NextScheduledStop(project.stored.schedule, lastScheduleCheckUnixMs_, *ports_.timeZone);
        for (const auto& at : {start, stop}) {
            if (at && (!nextScheduleEventUnixMs_ || *at < *nextScheduleEventUnixMs_)) {
                nextScheduleEventUnixMs_ = at;
            }
        }
        if (start && (!nextStart || *start < *nextStart)) {
            nextStart = start;
        }
    }
    if (ports_.wakeTimer == nullptr || nextStart == armedWakeUnixMs_) {
        return;
    }
    armedWakeUnixMs_ = nextStart;
    if (nextStart) {
        ports_.wakeTimer->Arm(*nextStart);
    } else {
        ports_.wakeTimer->Disarm();
    }
}

// ---------------------------------------------------------------------------------------------
// Items and state

GrabberExplorer::Project* GrabberExplorer::Find(model::GrabberProjectId id)
{
    const auto it = projects_.find(id);
    return it == projects_.end() ? nullptr : &it->second;
}

void GrabberExplorer::Prepare(Project& project)
{
    project.start = core::ParseUrl(project.stored.definition.startPage);
    if (project.start) {
        project.site = BaseSiteOf(*project.start);
        project.startIsPrivate = core::IsPrivateHost(project.start->host);
    }
}

bool GrabberExplorer::LoadItems(Project& project)
{
    if (project.itemsLoaded) {
        return true;
    }
    auto loaded = ports_.store->LoadGrabberItems(project.stored.id);
    if (!loaded) {
        ReportError("The project's list could not be loaded: " + loaded.GetError().message);
        return false;
    }
    project.items.clear();
    project.byAddress.clear();
    std::vector<model::GrabberItem> rows = std::move(loaded).Value();
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    const bool exact = !project.stored.definition.files.hideDuplicates;
    for (model::GrabberItem& row : rows) {
        // Ids are 1, 2, 3, ...; a damaged list with gaps or repeats keeps what is in order.
        if (row.id != static_cast<std::int64_t>(project.items.size()) + 1) {
            continue;
        }
        if (row.state == model::GrabberItemState::Downloading) {
            row.state = model::GrabberItemState::Found;  // LDM stopped while it downloaded
        }
        const auto url = core::ParseUrl(row.url);
        std::string key = url ? (exact ? row.url : NormalizeAddress(*url)) : row.url;
        Item item;
        item.data = std::move(row);
        project.byAddress.emplace(std::move(key), item.data.id);
        project.items.push_back(std::move(item));
    }
    project.itemsLoaded = true;
    return true;
}

void GrabberExplorer::UnloadItemsIfIdle(Project& project)
{
    // Pending adds name items by id; their answers need the list.
    if (project.open || project.exploring || project.downloading || !project.adding.empty() || !project.itemsLoaded) {
        return;
    }
    SaveItems(project);
    if (!project.unsaved.empty()) {
        return;  // keep what could not be saved
    }
    project.items.clear();
    project.items.shrink_to_fit();
    project.byAddress.clear();
    project.itemsLoaded = false;
}

GrabberExplorer::Item* GrabberExplorer::FindItem(Project& project, std::int64_t id)
{
    if (id < 1 || id > static_cast<std::int64_t>(project.items.size())) {
        return nullptr;
    }
    return &project.items[static_cast<std::size_t>(id - 1)];
}

GrabberExplorer::Item& GrabberExplorer::NewItem(Project& project, model::GrabberItem data, std::string key)
{
    data.id = static_cast<std::int64_t>(project.items.size()) + 1;
    Item item;
    item.data = std::move(data);
    project.byAddress.emplace(std::move(key), item.data.id);
    project.items.push_back(std::move(item));
    Item& added = project.items.back();
    project.unsaved.insert(added.data.id);
    snapshotDirty_ = true;
    return added;
}

void GrabberExplorer::Changed(Project& project, Item& item)
{
    item.viewDirty = true;
    project.unsaved.insert(item.data.id);
    snapshotDirty_ = true;
}

void GrabberExplorer::SaveItems(Project& project)
{
    if (project.unsaved.empty()) {
        return;
    }
    std::vector<model::GrabberItem> rows;
    rows.reserve(project.unsaved.size());
    for (const std::int64_t id : project.unsaved) {
        if (const Item* item = FindItem(project, id)) {
            rows.push_back(item->data);
        }
    }
    if (base::Status status = ports_.store->SaveGrabberItems(project.stored.id, rows); !status) {
        ReportError("The project's list could not be saved: " + status.GetError().message);
        return;
    }
    project.unsaved.clear();
}

void GrabberExplorer::SaveProjectRow(Project& project)
{
    if (project.damaged) {
        return;  // its stored definition is kept as it is
    }
    if (base::Status status =
            ports_.store->SaveGrabberProject(project.stored, DefinitionToJson(project.stored.definition));
        !status) {
        ReportError("The project could not be saved: " + status.GetError().message);
    }
    snapshotDirty_ = true;
}

std::optional<core::TransferCredentials> GrabberExplorer::CredentialsFor(const Project& project,
                                                                         const core::Url& url) const
{
    if (project.stored.loginUser.empty() || url.host != project.site.host) {
        return std::nullopt;
    }
    core::TransferCredentials credentials;
    credentials.user = project.stored.loginUser;
    if (!project.stored.loginSecret.empty()) {
        auto password = ports_.secrets->Unprotect(project.stored.loginSecret);
        if (!password) {
            return std::nullopt;
        }
        credentials.password = std::move(password).Value();
    }
    return credentials;
}

void GrabberExplorer::ReportError(std::string message)
{
    ports_.observer->OnGrabberNotification(GrabberError{std::move(message)});
}

GrabberStats GrabberExplorer::StatsOf(const Project& project) const
{
    GrabberStats stats;
    stats.limitReached = project.limitReached;
    stats.pagesExplored = project.pagesExplored;
    stats.pagesWaiting = static_cast<int>(project.pageQueue.size()) + project.fetching;
    for (const Item& item : project.items) {
        const model::GrabberItem& data = item.data;
        if (data.kind == model::GrabberItemKind::Page) {
            if (data.state == model::GrabberItemState::Failed) {
                ++stats.pagesFailed;
            }
            continue;
        }
        if (data.state == model::GrabberItemState::Filtered) {
            continue;
        }
        ++stats.filesFound;
        stats.filesChecked += data.checked ? 1 : 0;
        if (data.state == model::GrabberItemState::Done) {
            ++stats.filesDone;
            stats.bytesDone += data.size.value_or(0);
        } else if (data.state == model::GrabberItemState::Failed) {
            ++stats.filesFailed;
        }
    }
    return stats;
}

void GrabberExplorer::Publish()
{
    auto snapshot = std::make_shared<GrabberSnapshot>();
    snapshot->version = ++snapshotVersion_;
    std::vector<Project*> ordered;
    for (auto& [id, project] : projects_) {
        ordered.push_back(&project);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const Project* a, const Project* b) { return a->stored.name < b->stored.name; });
    for (Project* found : ordered) {
        Project& project = *found;
        GrabberProjectView view;
        view.project = project.stored;
        view.hasPassword = !project.stored.loginSecret.empty();
        view.project.loginSecret.clear();
        view.damaged = project.damaged;
        view.startPageFailure = project.startPageFailure;
        view.open = project.open;
        view.exploring = project.exploring;
        view.downloading = project.downloading;
        view.stats = StatsOf(project);
        if (project.open) {
            view.items.reserve(project.items.size());
            for (Item& item : project.items) {
                if (item.viewDirty || !item.view) {
                    item.view = std::make_shared<const model::GrabberItem>(item.data);
                    item.viewDirty = false;
                }
                view.items.push_back(item.view);
            }
        }
        snapshot->projects.push_back(std::move(view));
    }
    snapshotDirty_ = false;
    lastSnapshot_ = ports_.clock->Steady();
    ports_.observer->OnGrabberSnapshot(std::move(snapshot));
}

// ---------------------------------------------------------------------------------------------

void CoreOutputTee::OnSnapshot(std::shared_ptr<const core::Snapshot> snapshot)
{
    explorer_.OnCoreSnapshot(snapshot);
    frontEnd_.OnSnapshot(std::move(snapshot));
}

void CoreOutputTee::OnNotification(core::Notification notification)
{
    if (explorer_.TakeNotification(notification)) {
        return;
    }
    frontEnd_.OnNotification(std::move(notification));
}

}  // namespace ldm::grabber
