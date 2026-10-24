#pragma once

#include "model/Download.h"
#include "model/Ids.h"
#include "model/Queue.h"

#include <cstdint>
#include <vector>

// Which members of a running download queue start next, and when the queue is done (M4 design
// section 3.2). Pure: the coordinator describes the members and carries the decisions out.
namespace ldm::core {

struct QueueMember {
    model::DownloadId id;
    std::int64_t position = 0;
    model::DownloadStatus status = model::DownloadStatus::Queued;
    int failures = 0;  // failed attempts in the queue's current run
    // A run, the final move or the media tools are under way, or the quota holds the download:
    // it takes one of the queue's slots and the queue is not done while it lasts.
    bool busy = false;
    // Must not start now: its info dialog is open, it is being deleted, the user stopped it during
    // this run, or it waits for a new address from the browser.
    bool held = false;
};

// A failed member is tried again while it failed at most `retriesPerFile` times in this run, so
// a queue makes 1 + retriesPerFile attempts at each file.
[[nodiscard]] bool HasRetriesLeft(const model::Queue& queue, int failures) noexcept;

// The queue would start this member when a slot is free. A completed member of an ordinary queue
// is downloaded again (M4 design decision 3); the synchronization queue does not download its
// completed members again.
[[nodiscard]] bool IsStartable(const model::Queue& queue, const QueueMember& member) noexcept;

struct QueuePlan {
    std::vector<model::DownloadId> start;  // in queue order
    // No member is busy and none can start: the queue stops by itself.
    bool finished = false;
};

// `startsAllowed` is false while the download quota stops new runs: nothing starts, and members
// that could start keep the queue from being done.
[[nodiscard]] QueuePlan PlanQueue(const model::Queue& queue, std::vector<QueueMember> members, bool startsAllowed);

}  // namespace ldm::core
