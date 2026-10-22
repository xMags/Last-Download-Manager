#pragma once

#include "core/Snapshot.h"

#include <span>

namespace ldm::ui {

// Which list and toolbar commands apply to the current selection (checklist TB-01, L-05).
struct CommandState {
    bool resume = false;      // at least one selected download can start
    bool stop = false;        // at least one selected download is running
    bool remove = false;      // something is selected
    bool redownload = false;  // exactly one, not being deleted
    bool open = false;        // exactly one, completed
    bool openFolder = false;  // exactly one
    bool moveRename = false;  // exactly one, not running
    bool properties = false;  // exactly one
    bool stopAll = false;     // anything is running
    bool deleteCompleted = false;  // anything is completed
};

[[nodiscard]] CommandState ComputeCommandState(std::span<const core::DownloadView* const> selected,
                                               const core::Snapshot& snapshot);

}  // namespace ldm::ui
