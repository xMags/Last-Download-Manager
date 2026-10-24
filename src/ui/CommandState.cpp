#include "ui/CommandState.h"

#include "ui/StopChoice.h"

#include <algorithm>

namespace ldm::ui {

namespace {

using model::DownloadStatus;

bool Startable(const core::DownloadView& view)
{
    const DownloadStatus status = view.download.status;
    return status == DownloadStatus::Queued || status == DownloadStatus::Paused || status == DownloadStatus::Failed;
}

bool Running(const core::DownloadView& view)
{
    return view.download.status == DownloadStatus::Downloading;
}

}  // namespace

CommandState ComputeCommandState(std::span<const core::DownloadView* const> selected, const core::Snapshot& snapshot)
{
    CommandState state;
    const bool single = selected.size() == 1;
    const core::DownloadView* only = single ? selected.front() : nullptr;
    state.resume = std::any_of(selected.begin(), selected.end(), [](const auto* v) { return Startable(*v); });
    // A paused live recording can be stopped too: Stop is where it is finished (checklist P-07).
    state.stop = std::any_of(selected.begin(), selected.end(),
                             [](const auto* v) { return Running(*v) || IsLiveRecording(*v); });
    state.remove = std::any_of(selected.begin(), selected.end(),
                               [](const auto* v) { return v->download.status != DownloadStatus::Deleting; });
    if (only != nullptr) {
        const DownloadStatus status = only->download.status;
        state.redownload = status != DownloadStatus::Deleting && status != DownloadStatus::Finalizing;
        state.refreshAddress = state.redownload && status != DownloadStatus::Completed;
        state.open = status == DownloadStatus::Completed;
        state.openFolder = true;
        state.moveRename = status == DownloadStatus::Completed || Startable(*only);
        state.properties = true;
    }
    for (const auto& view : snapshot.downloads) {
        state.stopAll = state.stopAll || Running(*view);
        state.deleteCompleted = state.deleteCompleted || view->download.status == DownloadStatus::Completed;
    }
    return state;
}

}  // namespace ldm::ui
