#include "ui/StopChoice.h"

#include "base/Utf.h"
#include "ui/dialogs/MessageDialog.h"

namespace ldm::ui {

bool IsLiveRecording(const core::DownloadView& view)
{
    const model::DownloadStatus status = view.download.status;
    return view.stream && view.stream->live && status != model::DownloadStatus::Completed &&
           status != model::DownloadStatus::Finalizing && status != model::DownloadStatus::Deleting;
}

std::vector<core::Command> StopCommands(std::span<const core::DownloadView* const> selected,
                                        const std::function<RecordingStopAnswer(const std::wstring&)>& ask)
{
    std::vector<model::DownloadId> all;
    std::vector<model::DownloadId> live;
    std::vector<model::DownloadId> others;
    const core::DownloadView* firstLive = nullptr;
    for (const core::DownloadView* view : selected) {
        all.push_back(view->download.id);
        if (IsLiveRecording(*view)) {
            live.push_back(view->download.id);
            firstLive = firstLive != nullptr ? firstLive : view;
        } else {
            others.push_back(view->download.id);
        }
    }
    std::vector<core::Command> commands;
    if (live.empty()) {
        if (!all.empty()) {
            commands.emplace_back(core::StopCommand{std::move(all)});
        }
        return commands;
    }
    std::wstring question =
        live.size() == 1
            ? base::ToUtf16(firstLive->download.fileName) +
                  L" is a live stream. Save what was recorded so far as a complete file?"
            : std::to_wstring(live.size()) + L" of the selected downloads are live streams. Save what they recorded "
                                             L"so far as complete files?";
    question += L"\n\nYes finishes the recording and makes the file. No stops it; Resume continues recording later. "
                L"Cancel leaves it as it is.";
    switch (ask(question)) {
    case RecordingStopAnswer::Finish:
        commands.emplace_back(core::FinishRecordingCommand{std::move(live)});
        if (!others.empty()) {
            commands.emplace_back(core::StopCommand{std::move(others)});
        }
        break;
    case RecordingStopAnswer::Stop:
        commands.emplace_back(core::StopCommand{std::move(all)});
        break;
    case RecordingStopAnswer::Cancel:
        break;
    }
    return commands;
}

RecordingStopAnswer AskRecordingStop(HWND owner, const Theme& theme, const std::wstring& question)
{
    switch (ShowMessage(owner, theme, MessageRequest{question, MessageKind::Question, MessageButtons::YesNoCancel})) {
    case IDYES:
        return RecordingStopAnswer::Finish;
    case IDNO:
        return RecordingStopAnswer::Stop;
    default:
        return RecordingStopAnswer::Cancel;
    }
}

}  // namespace ldm::ui
