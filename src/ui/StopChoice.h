#pragma once

#include "core/Commands.h"
#include "core/Snapshot.h"
#include "ui/Theme.h"

#include <windows.h>

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace ldm::ui {

// The user's answer to Stop on a live recording (checklist P-07).
enum class RecordingStopAnswer {
    Finish,  // join what was recorded into the file
    Stop,    // stop; Resume records on later
    Cancel,  // leave it as it is
};

// A live recording that has not finished: Stop asks about it.
[[nodiscard]] bool IsLiveRecording(const core::DownloadView& view);

// What Stop does for the selected downloads. When live recordings are among them, `ask` gets one
// question for all of them; Finish ends those and stops the rest, Stop stops everything, Cancel
// does nothing. Without live recordings it stops them all without asking.
[[nodiscard]] std::vector<core::Command> StopCommands(std::span<const core::DownloadView* const> selected,
                                                      const std::function<RecordingStopAnswer(const std::wstring&)>& ask);

// Asks the question StopCommands passes, with Yes, No and Cancel.
[[nodiscard]] RecordingStopAnswer AskRecordingStop(HWND owner, const Theme& theme, const std::wstring& question);

}  // namespace ldm::ui
