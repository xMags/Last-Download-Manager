// Download queues (M4 design section 3): the coordinator's handling of queues and their members,
// kept apart from the rest of Coordinator.cpp. Which members start next is QueueRunner's decision.

#include "core/Coordinator.h"

#include "base/Utf.h"
#include "core/FailureText.h"
#include "core/Scheduler.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <string_view>
#include <utility>

namespace ldm::core {

namespace {

using model::DownloadStatus;

constexpr std::size_t kMaxQueueNameBytes = 100;

std::string Trimmed(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t");
    return std::string(text.substr(first, last - first + 1));
}

// The store compares names without regard to ASCII case (COLLATE NOCASE); so does this.
bool SameName(std::string_view a, std::string_view b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

std::optional<std::string> NameProblem(std::string_view name)
{
    if (name.empty()) {
        return "A queue needs a name.";
    }
    if (name.size() > kMaxQueueNameBytes) {
        return "The queue name is too long.";
    }
    if (std::any_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; })) {
        return "The queue name cannot contain control characters.";
    }
    return std::nullopt;
}

std::optional<std::string> ScheduleProblem(const model::Schedule& schedule)
{
    const auto timeOfDay = [](int minutes) { return minutes >= 0 && minutes < model::kMinutesPerDay; };
    if (!timeOfDay(schedule.startMinutes) || !timeOfDay(schedule.stopMinutes)) {
        return "The start and stop times must be times of day.";
    }
    if (schedule.weekdays > model::kEveryWeekday) {
        return "The days of the week are not valid.";
    }
    if (schedule.repeatMinutes < 0 || schedule.repeatMinutes > model::kMinutesPerDay) {
        return "The queue can start again every 0 to 1440 minutes.";
    }
    if (schedule.startDay && *schedule.startDay < 0) {
        return "The start day is not valid.";
    }
    if (schedule.enabled && schedule.mode == model::ScheduleMode::Once && !schedule.startDay) {
        return "Choose the day the queue starts.";
    }
    if (schedule.enabled && schedule.mode == model::ScheduleMode::Daily && schedule.weekdays == 0) {
        return "Choose at least one day of the week.";
    }
    return std::nullopt;
}

}  // namespace

void Coordinator::SaveQueue(model::Queue queue, const std::vector<model::DownloadId>& members)
{
    queue.name = Trimmed(queue.name);
    model::Queue* existing = queue.id.value == 0 ? nullptr : FindQueue(queue.id);
    if (queue.id.value != 0 && existing == nullptr) {
        return;  // deleted meanwhile
    }
    if (existing != nullptr && existing->IsBuiltin()) {
        queue.name = existing->name;  // the built-in queues keep their names
    }
    if (auto problem = NameProblem(queue.name)) {
        ReportError(std::move(*problem));
        return;
    }
    const bool taken = std::any_of(queues_.begin(), queues_.end(), [&queue](const model::Queue& other) {
        return other.id != queue.id && SameName(other.name, queue.name);
    });
    if (taken) {
        ReportError("A queue named \"" + queue.name + "\" already exists.");
        return;
    }
    if (queue.concurrent < 1 || queue.concurrent > model::kMaxQueueConcurrent) {
        ReportError("A queue downloads 1 to " + std::to_string(model::kMaxQueueConcurrent) + " files at a time.");
        return;
    }
    if (queue.retriesPerFile < 0 || queue.retriesPerFile > model::kMaxQueueRetries) {
        ReportError("A queue tries a file again 0 to " + std::to_string(model::kMaxQueueRetries) + " times.");
        return;
    }
    if (auto problem = ScheduleProblem(queue.schedule)) {
        ReportError(std::move(*problem));
        return;
    }
    if (!queue.openFileWhenDone.empty() &&
        !std::filesystem::path(base::ToUtf16(queue.openFileWhenDone)).is_absolute()) {
        ReportError("The file to open when the queue is done needs a full path.");
        return;
    }

    if (existing == nullptr) {
        queue.kind = model::QueueKind::Custom;
        queue.schedule.lastStartedAtMs.reset();
        std::int64_t order = 0;
        for (const model::Queue& other : queues_) {
            order = std::max(order, other.sortOrder + 1);
        }
        queue.sortOrder = order;
        auto id = ports_.store->InsertQueue(queue);
        if (!id) {
            ReportError("The queue could not be saved: " + id.GetError().message);
            return;
        }
        queue.id = id.Value();
        queues_.push_back(queue);
        snapshotDirty_ = true;
        PlanSchedules();
        if (!members.empty()) {
            SetQueue(members, queue.id);
        }
        return;
    }
    queue.kind = existing->kind;
    queue.sortOrder = existing->sortOrder;
    // When the schedule last fired is the coordinator's to record, not the front end's.
    queue.schedule.lastStartedAtMs = existing->schedule.lastStartedAtMs;
    if (base::Status saved = ports_.store->SaveQueue(queue); !saved) {
        ReportError("The queue could not be saved: " + saved.GetError().message);
        return;
    }
    *existing = std::move(queue);
    MarkMembersChanged(existing->id);  // the number of retries decides which members wait in it
    PlanSchedules();
}

void Coordinator::DeleteQueue(model::QueueId id)
{
    const model::Queue* queue = FindQueue(id);
    if (queue == nullptr) {
        return;
    }
    if (queue->IsBuiltin()) {
        ReportError("The built-in queues cannot be deleted.");
        return;
    }
    StopQueue(id);
    if (base::Status deleted = ports_.store->DeleteQueue(id); !deleted) {
        ReportError("The queue could not be deleted: " + deleted.GetError().message);
        return;
    }
    // The store cleared the members' queue; the entries follow.
    for (auto& [downloadId, entry] : entries_) {
        if (entry.download.queueId == id) {
            entry.download.queueId.reset();
            entry.download.queuePosition = 0;
            entry.download.queueFailures = 0;
            MarkChanged(entry);
        }
    }
    std::erase_if(queues_, [id](const model::Queue& q) { return q.id == id; });
    snapshotDirty_ = true;
    PlanSchedules();
}

void Coordinator::SetQueue(const std::vector<model::DownloadId>& ids, std::optional<model::QueueId> queue)
{
    if (queue && FindQueue(*queue) == nullptr) {
        ReportError("The selected queue no longer exists.");
        return;
    }
    // New members go to the end in the order of the list, whatever the order of the selection.
    std::vector<Entry*> moving;
    for (const model::DownloadId id : ids) {
        Entry* entry = Find(id);
        if (entry != nullptr && !entry->deleteWhenStopped && entry->download.queueId != queue &&
            std::find(moving.begin(), moving.end(), entry) == moving.end()) {
            moving.push_back(entry);
        }
    }
    std::sort(moving.begin(), moving.end(), [](const Entry* a, const Entry* b) {
        return std::pair(a->download.sortOrder, a->download.id.value) < std::pair(b->download.sortOrder, b->download.id.value);
    });
    std::int64_t position = queue ? NextQueuePosition(*queue) : 0;
    for (Entry* entry : moving) {
        model::Download& d = entry->download;
        if (d.queueId) {
            if (const auto run = queueRuns_.find(*d.queueId); run != queueRuns_.end()) {
                run->second.stoppedByUser.erase(d.id);
            }
        }
        d.queueId = queue;
        d.queuePosition = queue ? position++ : 0;
        d.queueFailures = 0;
        Save(*entry);
        MarkChanged(*entry);
    }
}

void Coordinator::ReorderQueue(model::QueueId id, const std::vector<model::DownloadId>& order)
{
    if (FindQueue(id) == nullptr) {
        return;
    }
    std::vector<Entry*> members;
    for (auto& [downloadId, entry] : entries_) {
        if (entry.download.queueId == id) {
            members.push_back(&entry);
        }
    }
    std::sort(members.begin(), members.end(), [](const Entry* a, const Entry* b) {
        return std::pair(a->download.queuePosition, a->download.id.value) <
               std::pair(b->download.queuePosition, b->download.id.value);
    });
    std::vector<Entry*> ordered;
    ordered.reserve(members.size());
    for (const model::DownloadId listed : order) {
        const auto member = std::find_if(members.begin(), members.end(),
                                         [listed](const Entry* e) { return e->download.id == listed; });
        if (member != members.end() && std::find(ordered.begin(), ordered.end(), *member) == ordered.end()) {
            ordered.push_back(*member);
        }
    }
    for (Entry* member : members) {
        if (std::find(ordered.begin(), ordered.end(), member) == ordered.end()) {
            ordered.push_back(member);
        }
    }
    std::int64_t position = 1;
    for (Entry* member : ordered) {
        if (member->download.queuePosition != position) {
            member->download.queuePosition = position;
            Save(*member);
            MarkChanged(*member);
        }
        position += 1;
    }
}

void Coordinator::StartQueue(model::QueueId id)
{
    const model::Queue* queue = FindQueue(id);
    if (queue == nullptr || queueRuns_.contains(id)) {
        return;
    }
    for (auto& [downloadId, entry] : entries_) {
        if (entry.download.queueId == id && entry.download.queueFailures != 0) {
            entry.download.queueFailures = 0;  // each run gives every member its retries again
            Save(entry);
        }
    }
    queueRuns_.emplace(id, QueueRun{});
    Notify(QueueStarted{id, queue->name});
    MarkMembersChanged(id);
    RunQueues();
}

void Coordinator::StopQueue(model::QueueId id, bool quietly)
{
    const model::Queue* queue = FindQueue(id);
    if (queue == nullptr || queueRuns_.erase(id) == 0) {
        return;
    }
    const std::string name = queue->name;
    std::vector<model::DownloadId> members;
    for (const auto& [downloadId, entry] : entries_) {
        if (entry.download.queueId == id && (entry.running || entry.quotaHeld)) {
            members.push_back(downloadId);
        }
    }
    for (const model::DownloadId member : members) {
        Entry* entry = Find(member);  // a stop can remove an entry that was being deleted
        if (entry == nullptr) {
            continue;
        }
        if (entry->quotaHeld) {
            entry->quotaHeld = false;  // it no longer starts by itself when the quota allows
            entry->download.lastResult.reset();
            Save(*entry);
            MarkChanged(*entry);
        }
        if (entry->running) {
            RequestStop(*entry);
        }
    }
    MarkMembersChanged(id);
    if (!quietly) {
        Notify(QueueStopped{id, name});
    }
}

void Coordinator::RunQueues()
{
    if (queueRuns_.empty() || shuttingDown_) {
        return;
    }
    std::map<model::QueueId, std::vector<QueueMember>> members;
    for (const auto& [downloadId, entry] : entries_) {
        if (entry.download.queueId && queueRuns_.contains(*entry.download.queueId)) {
            members[*entry.download.queueId].push_back(DescribeMember(entry));
        }
    }
    const bool startsAllowed = !QuotaBlocks(ports_.clock->Steady());
    std::vector<model::QueueId> running;
    for (const auto& [id, run] : queueRuns_) {
        running.push_back(id);
    }
    for (const model::QueueId id : running) {
        const model::Queue* queue = FindQueue(id);
        const auto run = queueRuns_.find(id);
        if (queue == nullptr || run == queueRuns_.end()) {
            queueRuns_.erase(id);
            continue;
        }
        const QueuePlan plan = PlanQueue(*queue, std::move(members[id]), startsAllowed);
        for (const model::DownloadId start : plan.start) {
            Entry* entry = Find(start);
            if (entry == nullptr) {
                continue;
            }
            run->second.startedAny = true;
            if (queue->kind == model::QueueKind::Sync) {
                run->second.started.insert(start);
            }
            if (entry->download.status != DownloadStatus::Completed) {
                Start(*entry);
            } else if (queue->kind == model::QueueKind::Sync) {
                StartSyncCheck(*entry);
            } else {
                Redownload(start);  // M4 design decision 3: the file is replaced once the new one is complete
            }
        }
        if (!plan.finished) {
            continue;
        }
        QueueFinished finished;
        finished.id = id;
        finished.name = queue->name;
        const bool userHeldWork = std::any_of(run->second.stoppedByUser.begin(), run->second.stoppedByUser.end(),
                                              [this](model::DownloadId member) {
                                                  const Entry* entry = Find(member);
                                                  return entry != nullptr &&
                                                         entry->download.status != DownloadStatus::Completed;
                                              });
        if (run->second.startedAny && run->second.completedAny && !userHeldWork) {
            finished.completionAction = queue->completionAction;
            finished.forceTerminate = queue->forceTerminate;
            finished.openFileWhenDone = queue->openFileWhenDone;
        }
        queueRuns_.erase(run);
        MarkMembersChanged(id);
        Notify(std::move(finished));
    }
}

void Coordinator::StartSyncCheck(Entry& entry)
{
    model::Download& d = entry.download;
    Entry::SyncCheck check{d.segments, d.totalSize, d.resumable, d.validators, d.finalUrl, d.completedAtMs};
    ResetProgress(entry);
    entry.syncCheck = std::move(check);
    entry.download.replaceFinalFile = true;  // a changed file replaces the old one once it is complete
    entry.automaticRestarts = 0;
    entry.forcedMaxConnections.reset();
    d.status = DownloadStatus::Queued;
    Start(entry);
    if (!entry.running && entry.syncCheck) {
        // The quota holds the run: the completed download stays as it was. (A run that could not
        // begin at all was put back by Fail.)
        entry.quotaHeld = false;
        (void)EndSyncCheck(entry, TransferOutcome::Stopped, std::nullopt);
    }
}

bool Coordinator::EndSyncCheck(Entry& entry, TransferOutcome outcome, const std::optional<TransferFailure>& failure)
{
    model::Download& d = entry.download;
    const std::optional<Entry::SyncCheck> check = std::exchange(entry.syncCheck, std::nullopt);
    if (!check || outcome == TransferOutcome::Completed) {
        return false;  // a changed file arrived whole: it completes like any download
    }
    if (outcome != TransferOutcome::Unchanged && d.DownloadedBytes() > 0) {
        return false;  // part of a changed file arrived: it continues like an interrupted Redownload
    }
    if (d.partialPath) {
        if (base::Status removed = ports_.fileSystem->Remove(std::filesystem::path(base::ToUtf16(*d.partialPath)));
            !removed) {
            ReportError("A partial file could not be deleted: " + removed.GetError().message);
        }
        d.partialPath.reset();
    }
    d.segments = check->segments;
    d.totalSize = check->totalSize;
    d.resumable = check->resumable;
    d.validators = check->validators;
    d.finalUrl = check->finalUrl;
    d.completedAtMs = check->completedAtMs;
    d.status = DownloadStatus::Completed;
    entry.download.replaceFinalFile = false;
    entry.receivedBytes = d.DownloadedBytes();
    if (outcome == TransferOutcome::Unchanged) {
        d.lastResult = "Unchanged on the server";
        NoteQueueWorkDone(entry);
    } else if (failure) {
        d.lastResult = "Could not check the server for changes: " + DescribeFailure(*failure);
    } else {
        d.lastResult = "The check for changes was stopped";
    }
    // Moved to an ordinary queue while it was checked: a completed download leaves it, as it would
    // have had it completed there, instead of being downloaded again by that queue.
    LeaveQueueOnCompletion(entry);
    Save(entry);
    MarkChanged(entry);
    return true;
}

void Coordinator::StartQueuesOnAppStart()
{
    std::vector<model::QueueId> starting;
    for (const model::Queue& queue : queues_) {
        if (queue.schedule.startOnAppStart) {
            starting.push_back(queue.id);
        }
    }
    for (const model::QueueId id : starting) {
        StartQueue(id);
    }
}

void Coordinator::CheckSchedules()
{
    if (ports_.timeZone == nullptr) {
        return;
    }
    const std::int64_t now = ports_.clock->NowUnixMs();
    if (now < lastScheduleCheckUnixMs_) {
        // The clock went back: what lies between now and the last check was handled already.
        lastScheduleCheckUnixMs_ = now;
        PlanSchedules();
        return;
    }
    const std::int64_t after = std::max(lastScheduleCheckUnixMs_, now - kScheduleGraceMs);
    lastScheduleCheckUnixMs_ = now;
    std::vector<model::QueueId> ids;
    for (const model::Queue& queue : queues_) {
        ids.push_back(queue.id);
    }
    for (const model::QueueId id : ids) {
        model::Queue* queue = FindQueue(id);
        if (queue == nullptr) {
            continue;
        }
        const model::Schedule& schedule = queue->schedule;
        auto start = NextScheduledStart(schedule, after, *ports_.timeZone);
        auto stop = NextScheduledStop(schedule, after, *ports_.timeZone);
        if (start && (*start > now || (schedule.mode == model::ScheduleMode::Once && schedule.lastStartedAtMs &&
                                       *schedule.lastStartedAtMs >= *start))) {
            start.reset();  // not yet, or a single start that already happened before the clock moved
        }
        if (stop && *stop > now) {
            stop.reset();
        }
        if (start && stop && *stop > *start) {
            // Both passed while LDM could not look, the stop after the start: the queue ends stopped.
            if (queueRuns_.contains(id)) {
                StopQueue(id);
            }
            continue;
        }
        // A stop at the moment of a start ends the previous run ("stop time" equal to the start
        // time means the next day), so it comes first.
        if (stop) {
            StopQueue(id);
        }
        if (!start) {
            continue;
        }
        queue->schedule.lastStartedAtMs = now;
        if (base::Status saved = ports_.store->SaveQueue(*queue); !saved) {
            ReportError("The queue could not be saved: " + saved.GetError().message);
        }
        snapshotDirty_ = true;
        StartQueue(id);
    }
    PlanSchedules();
}

void Coordinator::PlanSchedules()
{
    nextScheduleEventUnixMs_.reset();
    anyScheduleEnabled_ = false;
    std::optional<std::int64_t> nextStart;
    if (ports_.timeZone != nullptr) {
        for (const model::Queue& queue : queues_) {
            anyScheduleEnabled_ = anyScheduleEnabled_ || queue.schedule.enabled;
            const auto start = NextScheduledStart(queue.schedule, lastScheduleCheckUnixMs_, *ports_.timeZone);
            const auto stop = NextScheduledStop(queue.schedule, lastScheduleCheckUnixMs_, *ports_.timeZone);
            for (const auto& at : {start, stop}) {
                if (at && (!nextScheduleEventUnixMs_ || *at < *nextScheduleEventUnixMs_)) {
                    nextScheduleEventUnixMs_ = at;
                }
            }
            if (start && (!nextStart || *start < *nextStart)) {
                nextStart = start;
            }
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

model::Queue* Coordinator::FindQueue(model::QueueId id)
{
    const auto it = std::find_if(queues_.begin(), queues_.end(), [id](const model::Queue& q) { return q.id == id; });
    return it == queues_.end() ? nullptr : &*it;
}

std::optional<model::QueueId> Coordinator::MainQueue() const
{
    const auto it = std::find_if(queues_.begin(), queues_.end(),
                                 [](const model::Queue& q) { return q.kind == model::QueueKind::Main; });
    return it == queues_.end() ? std::nullopt : std::optional<model::QueueId>(it->id);
}

std::int64_t Coordinator::NextQueuePosition(model::QueueId id) const
{
    std::int64_t last = 0;
    for (const auto& [downloadId, entry] : entries_) {
        if (entry.download.queueId == id) {
            last = std::max(last, entry.download.queuePosition);
        }
    }
    return last + 1;
}

QueueMember Coordinator::DescribeMember(const Entry& entry) const
{
    const model::Download& d = entry.download;
    QueueMember member;
    member.id = d.id;
    member.position = d.queuePosition;
    member.status = d.status;
    member.failures = d.queueFailures;
    member.busy = entry.running || entry.finalizing || entry.muxing || entry.describing || entry.quotaHeld;
    bool stoppedByUser = false;
    bool startedInThisRun = false;
    if (d.queueId) {
        if (const auto run = queueRuns_.find(*d.queueId); run != queueRuns_.end()) {
            stoppedByUser = run->second.stoppedByUser.contains(d.id);
            startedInThisRun = run->second.started.contains(d.id);
        }
    }
    // The synchronization queue checks a completed member once per run, and only files: a stream
    // has no single file on the server to compare.
    const auto queue = std::find_if(queues_.begin(), queues_.end(),
                                    [&d](const model::Queue& q) { return d.queueId && q.id == *d.queueId; });
    const bool synchronized = queue != queues_.end() && queue->kind == model::QueueKind::Sync &&
                              d.status == DownloadStatus::Completed && (startedInThisRun || d.IsStream());
    member.held = d.awaitingConfirmation || entry.deleteWhenStopped || entry.holdFinalize ||
                  waitingForAddress_ == d.id || stoppedByUser || synchronized;
    return member;
}

bool Coordinator::IsWaitingInQueue(const Entry& entry) const
{
    const auto& queueId = entry.download.queueId;
    if (!queueId || !queueRuns_.contains(*queueId)) {
        return false;
    }
    const auto queue = std::find_if(queues_.begin(), queues_.end(),
                                    [&queueId](const model::Queue& q) { return q.id == *queueId; });
    return queue != queues_.end() && IsStartable(*queue, DescribeMember(entry));
}

void Coordinator::MarkMembersChanged(model::QueueId id)
{
    for (auto& [downloadId, entry] : entries_) {
        if (entry.download.queueId == id) {
            MarkChanged(entry);
        }
    }
    snapshotDirty_ = true;
}

void Coordinator::NoteQueueWorkDone(const Entry& entry)
{
    if (!entry.download.queueId) {
        return;
    }
    if (const auto run = queueRuns_.find(*entry.download.queueId); run != queueRuns_.end()) {
        run->second.completedAny = true;
    }
}

void Coordinator::LeaveQueueOnCompletion(Entry& entry)
{
    model::Download& d = entry.download;
    if (!d.queueId) {
        return;
    }
    const model::Queue* queue = FindQueue(*d.queueId);
    if (queue != nullptr && queue->kind == model::QueueKind::Sync) {
        return;  // the synchronization queue checks its completed members again on each run (Q-05)
    }
    d.queueId.reset();
    d.queuePosition = 0;
    d.queueFailures = 0;
    snapshotDirty_ = true;
}

void Coordinator::CountQueueFailure(Entry& entry)
{
    model::Download& d = entry.download;
    if (!d.queueId || !queueRuns_.contains(*d.queueId)) {
        return;
    }
    const model::Queue* queue = FindQueue(*d.queueId);
    if (queue == nullptr) {
        return;
    }
    d.queueFailures += 1;
    if (HasRetriesLeft(*queue, d.queueFailures)) {
        d.queuePosition = NextQueuePosition(*d.queueId);  // tried again after the others (Q-06)
    }
}

}  // namespace ldm::core
