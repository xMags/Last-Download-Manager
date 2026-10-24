#pragma once

#include "core/Snapshot.h"
#include "model/Ids.h"
#include "model/Queue.h"

#include <optional>
#include <span>
#include <vector>

// What Add to queue and the queue menus act on (M4 design section 7). Pure, for tests.
namespace ldm::ui {

// The selected downloads that Add to queue moves into `target`: those not in it already and not
// being deleted. A completed one joins an ordinary queue only with `includeCompleted`, since it is
// downloaded again when its turn comes (M4 design decision 3); the synchronization queue only
// checks it, so it always joins that one.
[[nodiscard]] std::vector<model::DownloadId> QueueJoiners(std::span<const core::DownloadView* const> selected,
                                                          const model::Queue& target, bool includeCompleted);

// How many of the joiners would be downloaded again: the user is asked about them first.
[[nodiscard]] int CompletedJoiners(std::span<const core::DownloadView* const> selected, const model::Queue& target);

// The queue with this id in the snapshot, if it still exists.
[[nodiscard]] const core::QueueView* FindQueueView(const core::Snapshot& snapshot, model::QueueId id);
// The main download queue, which Download Later and --start-queue use.
[[nodiscard]] std::optional<model::QueueId> MainQueueId(const core::Snapshot& snapshot);

}  // namespace ldm::ui
