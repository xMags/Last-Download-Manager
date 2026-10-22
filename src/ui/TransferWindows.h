#pragma once

#include "core/Commands.h"
#include "core/Snapshot.h"
#include "ui/ShellActions.h"
#include "ui/SystemActions.h"
#include "ui/Theme.h"

#include <windows.h>

#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace ldm::ui {

class CompleteDialog;
class ProgressWindow;

// The windows that follow individual transfers (checklist P-01 to P-06, C-01) and what happens when
// one completes: sounds, the virus scanner and the power action (O-09, ENG-18, P-05).
class TransferWindows {
public:
    struct Context {
        HWND owner = nullptr;
        core::CommandSink* commands = nullptr;
        ShellActions* shell = nullptr;
        SystemActions* system = nullptr;
        const Theme* theme = nullptr;
        std::function<std::shared_ptr<const core::Snapshot>()> snapshot;
        std::function<void()> exit;
    };

    explicit TransferWindows(Context context);
    ~TransferWindows();
    TransferWindows(const TransferWindows&) = delete;
    TransferWindows& operator=(const TransferWindows&) = delete;

    // Opens (or brings forward) the progress window of a download, unless Options hide them.
    void OpenProgress(model::DownloadId id);
    void OnSnapshot(const core::Snapshot& snapshot);
    void OnCompleted(const core::DownloadCompleted& completed);
    void OnFailed(const core::DownloadFailed& failed);
    bool PreTranslate(MSG& message);

    // Open progress windows, for the tray menu (checklist V-01).
    [[nodiscard]] std::vector<std::pair<HWND, std::wstring>> Windows() const;

private:
    void RunCompletionAction(model::CompletionAction action);
    void ShowError(const std::wstring& message) const;

    Context context_;
    std::map<model::DownloadId, std::unique_ptr<ProgressWindow>> progress_;
    std::vector<std::unique_ptr<CompleteDialog>> completeDialogs_;
};

}  // namespace ldm::ui
