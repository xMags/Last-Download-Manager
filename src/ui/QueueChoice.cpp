#include "ui/QueueChoice.h"

#include <algorithm>

namespace ldm::ui {

namespace {

bool Joins(const core::DownloadView& view, const model::Queue& target)
{
    return view.download.queueId != target.id && view.download.status != model::DownloadStatus::Deleting;
}

bool DownloadsAgain(const core::DownloadView& view, const model::Queue& target)
{
    return view.download.status == model::DownloadStatus::Completed && target.kind != model::QueueKind::Sync;
}

}  // namespace

std::vector<model::DownloadId> QueueJoiners(std::span<const core::DownloadView* const> selected,
                                            const model::Queue& target, bool includeCompleted)
{
    std::vector<model::DownloadId> ids;
    for (const core::DownloadView* view : selected) {
        if (Joins(*view, target) && (includeCompleted || !DownloadsAgain(*view, target))) {
            ids.push_back(view->download.id);
        }
    }
    return ids;
}

int CompletedJoiners(std::span<const core::DownloadView* const> selected, const model::Queue& target)
{
    return static_cast<int>(std::count_if(selected.begin(), selected.end(), [&target](const core::DownloadView* view) {
        return Joins(*view, target) && DownloadsAgain(*view, target);
    }));
}

const core::QueueView* FindQueueView(const core::Snapshot& snapshot, model::QueueId id)
{
    const auto it = std::find_if(snapshot.queues.begin(), snapshot.queues.end(),
                                 [id](const core::QueueView& view) { return view.queue.id == id; });
    return it == snapshot.queues.end() ? nullptr : &*it;
}

std::optional<model::QueueId> MainQueueId(const core::Snapshot& snapshot)
{
    const auto it = std::find_if(snapshot.queues.begin(), snapshot.queues.end(),
                                 [](const core::QueueView& view) { return view.queue.kind == model::QueueKind::Main; });
    return it == snapshot.queues.end() ? std::nullopt : std::optional<model::QueueId>(it->queue.id);
}

}  // namespace ldm::ui
