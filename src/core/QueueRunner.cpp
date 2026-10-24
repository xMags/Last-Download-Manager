#include "core/QueueRunner.h"

#include <algorithm>
#include <utility>

namespace ldm::core {

bool HasRetriesLeft(const model::Queue& queue, int failures) noexcept
{
    return failures <= queue.retriesPerFile;
}

bool IsStartable(const model::Queue& queue, const QueueMember& member) noexcept
{
    if (member.busy || member.held) {
        return false;
    }
    switch (member.status) {
    case model::DownloadStatus::Queued:
    case model::DownloadStatus::Paused:
    case model::DownloadStatus::Completed:
        return true;
    case model::DownloadStatus::Failed:
        return HasRetriesLeft(queue, member.failures);
    default:
        return false;
    }
}

QueuePlan PlanQueue(const model::Queue& queue, std::vector<QueueMember> members, bool startsAllowed)
{
    std::sort(members.begin(), members.end(), [](const QueueMember& a, const QueueMember& b) {
        return std::pair(a.position, a.id.value) < std::pair(b.position, b.id.value);
    });
    const auto busy = std::count_if(members.begin(), members.end(), [](const QueueMember& m) { return m.busy; });
    std::int64_t free = std::max<std::int64_t>(0, std::int64_t{queue.concurrent} - busy);
    bool anyStartable = false;
    QueuePlan plan;
    for (const QueueMember& member : members) {
        if (!IsStartable(queue, member)) {
            continue;
        }
        anyStartable = true;
        if (startsAllowed && free > 0) {
            plan.start.push_back(member.id);
            free -= 1;
        }
    }
    plan.finished = busy == 0 && !anyStartable;
    return plan;
}

}  // namespace ldm::core
