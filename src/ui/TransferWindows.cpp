#include "ui/TransferWindows.h"

#include "base/Utf.h"
#include "ui/dialogs/CompleteDialog.h"
#include "ui/dialogs/MessageDialog.h"
#include "ui/dialogs/ProgressWindow.h"

#include <algorithm>
#include <filesystem>

namespace ldm::ui {

namespace {

constexpr int kPowerActionCountdownSeconds = 30;

}  // namespace

TransferWindows::TransferWindows(Context context) : context_(std::move(context)) {}

TransferWindows::~TransferWindows() = default;

bool TransferWindows::PreTranslate(MSG& message)
{
    for (const auto& [id, window] : progress_) {
        if (window->Handle() != nullptr && ::IsDialogMessageW(window->Handle(), &message)) {
            return true;
        }
    }
    for (const auto& dialog : completeDialogs_) {
        if (dialog->Handle() != nullptr && ::IsDialogMessageW(dialog->Handle(), &message)) {
            return true;
        }
    }
    return false;
}

std::vector<std::pair<HWND, std::wstring>> TransferWindows::Windows() const
{
    std::vector<std::pair<HWND, std::wstring>> windows;
    for (const auto& [id, window] : progress_) {
        if (window->Handle() == nullptr) {
            continue;
        }
        wchar_t title[256] = {};
        ::GetWindowTextW(window->Handle(), title, static_cast<int>(std::size(title)));
        windows.emplace_back(window->Handle(), title);
    }
    return windows;
}

void TransferWindows::OpenProgress(model::DownloadId id)
{
    const auto snapshot = context_.snapshot();
    if (!snapshot) {
        return;
    }
    if (const auto existing = progress_.find(id); existing != progress_.end()) {
        if (existing->second->Handle() != nullptr) {
            ::ShowWindow(existing->second->Handle(), SW_RESTORE);
            ::SetForegroundWindow(existing->second->Handle());
        }
        return;
    }
    if (snapshot->settings.progressStartView == model::ProgressStartView::Hidden) {
        return;
    }
    const auto view = std::find_if(snapshot->downloads.begin(), snapshot->downloads.end(),
                                   [id](const auto& v) { return v->download.id == id; });
    if (view == snapshot->downloads.end()) {
        return;
    }
    // The closed callback runs from the window's WM_DESTROY, after which the dialog procedure
    // touches nothing of the object, so erasing it there is safe. Nothing iterates progress_
    // while windows are being destroyed (see OnSnapshot).
    auto window = std::make_unique<ProgressWindow>(
        *context_.theme, id,
        ProgressWindow::Callbacks{[this](core::Command command) { context_.commands->Post(std::move(command)); },
                                  [this](model::DownloadId closed) { progress_.erase(closed); }});
    ProgressWindow& created = *window;
    progress_.emplace(id, std::move(window));
    created.Open(context_.owner, **view, snapshot->settings);
}

void TransferWindows::OnSnapshot(const core::Snapshot& snapshot)
{
    std::vector<HWND> gone;
    for (const auto& [id, window] : progress_) {
        const auto view = std::find_if(snapshot.downloads.begin(), snapshot.downloads.end(),
                                       [id = id](const auto& v) { return v->download.id == id; });
        if (view == snapshot.downloads.end() || (*view)->download.status == model::DownloadStatus::Deleting) {
            gone.push_back(window->Handle());
            continue;
        }
        window->Update(**view, snapshot.settings);
    }
    // Destroying a window erases it from progress_, so it happens after the loop.
    for (const HWND window : gone) {
        if (window != nullptr) {
            ::DestroyWindow(window);
        }
    }
}

void TransferWindows::OnCompleted(const core::DownloadCompleted& completed)
{
    if (const auto it = progress_.find(completed.id); it != progress_.end() && it->second->Handle() != nullptr) {
        ::DestroyWindow(it->second->Handle());  // erases itself from progress_
    }
    const auto snapshot = context_.snapshot();
    if (!snapshot) {
        return;
    }
    const model::Settings& settings = snapshot->settings;
    if (settings.soundComplete.enabled && !settings.soundComplete.file.empty()) {
        context_.system->PlaySoundFile(std::filesystem::path(base::ToUtf16(settings.soundComplete.file)));
    }
    if (!settings.virusScan.program.empty()) {
        const std::wstring arguments =
            SubstituteFileArgument(base::ToUtf16(settings.virusScan.arguments), base::ToUtf16(completed.finalPath));
        if (!context_.system->StartProgram(std::filesystem::path(base::ToUtf16(settings.virusScan.program)), arguments)) {
            ShowError(L"The virus scanner could not be started. Check Options > Downloads.");
        }
    }
    if (settings.showCompleteDialog) {
        auto dialog = std::make_unique<CompleteDialog>(
            *context_.theme, *context_.shell, completed,
            CompleteDialog::Callbacks{
                [this] {
                    context_.commands->Post(
                        core::ModifySettingsCommand{[](model::Settings& s) { s.showCompleteDialog = false; }});
                },
                [this](CompleteDialog* closed) {
                    std::erase_if(completeDialogs_, [closed](const auto& d) { return d.get() == closed; });
                }});
        CompleteDialog& created = *dialog;
        completeDialogs_.push_back(std::move(dialog));
        created.Open(context_.owner);
    }
    RunCompletionAction(completed.completionAction);
}

void TransferWindows::OnFailed(const core::DownloadFailed& failed)
{
    (void)failed;
    const auto snapshot = context_.snapshot();
    if (snapshot && snapshot->settings.soundFailed.enabled && !snapshot->settings.soundFailed.file.empty()) {
        context_.system->PlaySoundFile(std::filesystem::path(base::ToUtf16(snapshot->settings.soundFailed.file)));
    }
}

void TransferWindows::RunCompletionAction(model::CompletionAction action)
{
    if (action == model::CompletionAction::None) {
        return;
    }
    const wchar_t* text = action == model::CompletionAction::ExitApp    ? L"exit"
                          : action == model::CompletionAction::Shutdown ? L"shut down the computer"
                          : action == model::CompletionAction::Sleep    ? L"put the computer to sleep"
                                                                        : L"hibernate the computer";
    CountdownDialog countdown(*context_.theme, text, kPowerActionCountdownSeconds);
    if (!countdown.Run(context_.owner)) {
        return;
    }
    bool ok = true;
    switch (action) {
    case model::CompletionAction::ExitApp:
        context_.exit();
        return;
    case model::CompletionAction::Shutdown:
        ok = context_.system->ShutDown();
        break;
    case model::CompletionAction::Sleep:
        ok = context_.system->Sleep();
        break;
    case model::CompletionAction::Hibernate:
        ok = context_.system->Hibernate();
        break;
    case model::CompletionAction::None:
        break;
    }
    if (!ok) {
        ShowError(L"Windows refused the power action.");
    }
}

void TransferWindows::ShowError(const std::wstring& message) const
{
    (void)ShowMessage(context_.owner, *context_.theme, MessageRequest{message, MessageKind::Warning});
}

}  // namespace ldm::ui
