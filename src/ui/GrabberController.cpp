#include "ui/GrabberController.h"

#include "base/Utf.h"
#include "ui/dialogs/GrabberDialogs.h"
#include "ui/dialogs/GrabberWindow.h"
#include "ui/dialogs/GrabberWizard.h"
#include "ui/dialogs/MessageDialog.h"

#include <type_traits>
#include <utility>

namespace ldm::ui {

GrabberController::GrabberController(Context context) : context_(std::move(context)) {}

GrabberController::~GrabberController()
{
    CloseAll();
}

void GrabberController::CloseAll()
{
    // Destroying a window removes it from windows_ (its closed callback), so go by a copy of the ids.
    std::vector<std::int64_t> ids;
    for (const auto& [id, window] : windows_) {
        ids.push_back(id);
    }
    for (const std::int64_t id : ids) {
        const auto it = windows_.find(id);
        if (it != windows_.end() && it->second->Handle() != nullptr) {
            ::DestroyWindow(it->second->Handle());
        }
    }
    windows_.clear();
}

void GrabberController::Post(core::GrabberCommand command)
{
    if (context_.grabber != nullptr) {
        context_.grabber->Post(std::move(command));
    }
}

const grabber::GrabberProjectView* GrabberController::Find(model::GrabberProjectId id) const
{
    if (!snapshot_) {
        return nullptr;
    }
    for (const grabber::GrabberProjectView& view : snapshot_->projects) {
        if (view.project.id == id) {
            return &view;
        }
    }
    return nullptr;
}

HWND GrabberController::DialogOwner() const
{
    const HWND active = ::GetActiveWindow();
    for (const auto& [id, window] : windows_) {
        if (window->Handle() != nullptr && window->Handle() == active) {
            return active;
        }
    }
    return context_.owner;
}

void GrabberController::Tell(const std::wstring& message, bool warning) const
{
    (void)ShowMessage(DialogOwner(), *context_.theme,
                      MessageRequest{message, warning ? MessageKind::Warning : MessageKind::Information});
}

void GrabberController::OpenChooser()
{
    if (chooser_ != nullptr) {
        ::SetForegroundWindow(chooser_->Handle());
        return;
    }
    GrabberProjectsDialog::Callbacks callbacks;
    callbacks.rename = [this](model::GrabberProjectId id, std::string name) {
        const grabber::GrabberProjectView* view = Find(id);
        if (view == nullptr) {
            return;
        }
        core::GrabberSaveProjectCommand save;
        save.token = nextToken_++;
        save.project = view->project;
        save.project.name = std::move(name);
        pendingSaves_.emplace(save.token, AfterSave::Nothing);
        Post(std::move(save));
    };
    callbacks.remove = [this](model::GrabberProjectId id) { Post(core::GrabberDeleteProjectCommand{id}); };
    GrabberProjectsDialog dialog(*context_.theme, snapshot_, std::move(callbacks));
    chooser_ = &dialog;
    const auto choice = dialog.Run(context_.owner);
    chooser_ = nullptr;
    if (!choice) {
        return;
    }
    switch (choice->action) {
    case GrabberProjectsDialog::Action::NewProject:
        NewProject(std::nullopt);
        break;
    case GrabberProjectsDialog::Action::NewFromTemplate:
        NewProject(choice->project);
        break;
    case GrabberProjectsDialog::Action::Open:
        if (choice->project) {
            OpenWindow(*choice->project);
        }
        break;
    case GrabberProjectsDialog::Action::Edit:
        if (choice->project) {
            EditProject(*choice->project);
        }
        break;
    }
}

void GrabberController::NewProject(std::optional<model::GrabberProjectId> fromTemplate)
{
    model::GrabberProject project;
    if (fromTemplate) {
        if (const grabber::GrabberProjectView* view = Find(*fromTemplate)) {
            project.definition = view->project.definition;
            project.loginUser = view->project.loginUser;
        }
    }
    RunWizard(std::move(project), false, AfterSave::OpenWindow);
}

void GrabberController::EditProject(model::GrabberProjectId id)
{
    const grabber::GrabberProjectView* view = Find(id);
    if (view == nullptr) {
        return;
    }
    if (view->exploring || view->downloading) {
        Tell(L"Stop the project before changing its settings.", false);
        return;
    }
    Post(core::GrabberEditingCommand{id, true});
    RunWizard(view->project, view->hasPassword, AfterSave::Nothing);
    Post(core::GrabberEditingCommand{id, false});
}

void GrabberController::RunWizard(model::GrabberProject project, bool hasPassword, AfterSave after)
{
    std::vector<model::Category> categories;
    if (context_.snapshot) {
        if (const auto core = context_.snapshot()) {
            categories = core->categories;
        }
    }
    const std::optional<model::GrabberProjectId> self =
        project.id.value != 0 ? std::optional(project.id) : std::nullopt;
    GrabberWizard wizard(*context_.theme, std::move(project), hasPassword, std::move(categories),
                         GrabberNames(snapshot_.get(), self));
    auto result = wizard.Run(DialogOwner());
    if (!result) {
        return;
    }
    core::GrabberSaveProjectCommand save;
    save.token = nextToken_++;
    save.project = std::move(result->project);
    if (self) {
        // The Scheduler may have changed the project's schedule while the wizard was open: only
        // what the wizard edits goes over the newest copy.
        const grabber::GrabberProjectView* latest = Find(*self);
        if (latest == nullptr) {
            return;  // deleted meanwhile
        }
        model::GrabberProject merged = latest->project;
        merged.name = std::move(save.project.name);
        merged.definition = std::move(save.project.definition);
        merged.loginUser = std::move(save.project.loginUser);
        save.project = std::move(merged);
    }
    save.loginPassword = std::move(result->password);
    pendingSaves_.emplace(save.token, save.project.isTemplate ? AfterSave::Nothing : after);
    Post(std::move(save));
}

void GrabberController::SaveAsTemplate(model::GrabberProjectId id)
{
    const grabber::GrabberProjectView* view = Find(id);
    if (view == nullptr) {
        return;
    }
    // Copied: a newer snapshot replaces `view` while the dialog is open.
    const model::GrabberProject source = view->project;
    GrabberNameDialog dialog(*context_.theme, L"Save as template", base::ToUtf16(source.name) + L" template",
                             GrabberNames(snapshot_.get(), std::nullopt));
    const auto name = dialog.Run(DialogOwner());
    if (!name) {
        return;
    }
    core::GrabberSaveProjectCommand save;
    save.token = nextToken_++;
    save.project.name = *name;
    save.project.isTemplate = true;
    save.project.definition = source.definition;
    save.project.loginUser = source.loginUser;
    pendingSaves_.emplace(save.token, AfterSave::Nothing);
    Post(std::move(save));
}

void GrabberController::OpenWindow(model::GrabberProjectId id)
{
    if (const auto open = windows_.find(id.value); open != windows_.end()) {
        open->second->Activate();
        return;
    }
    GrabberWindow::Callbacks callbacks;
    callbacks.post = [this](core::GrabberCommand command) { Post(std::move(command)); };
    callbacks.explore = [this](model::GrabberProjectId project, bool keepChecks) { Explore(project, keepChecks); };
    callbacks.edit = [this](model::GrabberProjectId project) { EditProject(project); };
    callbacks.saveAsTemplate = [this](model::GrabberProjectId project) { SaveAsTemplate(project); };
    callbacks.openFile = [this](const std::filesystem::path& file) {
        return context_.shell != nullptr && context_.shell->Open(DialogOwner(), file);
    };
    callbacks.showInFolder = [this](const std::filesystem::path& file) {
        return context_.shell != nullptr && context_.shell->ShowInFolder(file);
    };
    callbacks.closed = [this](model::GrabberProjectId project) {
        // Runs while the window object is being torn down; it is deleted once that returns.
        const auto it = windows_.find(project.value);
        if (it != windows_.end()) {
            closing_.push_back(std::move(it->second));
            windows_.erase(it);
        }
    };
    auto window = std::make_unique<GrabberWindow>(*context_.theme, id, std::move(callbacks));
    GrabberWindow* raw = window.get();
    windows_.emplace(id.value, std::move(window));
    raw->Open(context_.owner, snapshot_);
    if (raw->Handle() == nullptr) {
        windows_.erase(id.value);
    }
}

void GrabberController::Explore(model::GrabberProjectId id, bool keepChecks)
{
    const grabber::GrabberProjectView* view = Find(id);
    if (view == nullptr) {
        return;
    }
    if (!view->project.definition.useBrowserCookies) {
        Post(core::GrabberExploreCommand{id, keepChecks, {}});
        return;
    }
    const std::string startPage = view->project.definition.startPage;
    const bool asked = context_.requestCookies &&
                       context_.requestCookies(startPage, [this, id, keepChecks](std::optional<std::vector<model::Cookie>> cookies) {
                           if (!cookies) {
                               Tell(L"The browser did not send its cookies, so the site is explored without them.", true);
                           }
                           Post(core::GrabberExploreCommand{id, keepChecks, cookies.value_or(std::vector<model::Cookie>{})});
                       });
    if (!asked) {
        Tell(L"No browser is connected to LDM, so the site is explored without the browser's cookies.", true);
        Post(core::GrabberExploreCommand{id, keepChecks, {}});
    }
}

void GrabberController::OnSnapshot(std::shared_ptr<const grabber::GrabberSnapshot> snapshot)
{
    snapshot_ = std::move(snapshot);
    closing_.clear();
    if (chooser_ != nullptr) {
        chooser_->Update(snapshot_);
    }
    std::vector<std::int64_t> ids;
    for (const auto& [id, window] : windows_) {
        ids.push_back(id);
    }
    for (const std::int64_t id : ids) {
        if (const auto it = windows_.find(id); it != windows_.end()) {
            it->second->Update(snapshot_);  // may close the window when its project is gone
        }
    }
}

void GrabberController::OnCoreSnapshot(const core::Snapshot& snapshot)
{
    closing_.clear();
    for (const auto& [id, window] : windows_) {
        window->UpdateDownloads(snapshot);
    }
}

void GrabberController::OnNotifications(std::vector<grabber::GrabberNotification> notifications)
{
    for (grabber::GrabberNotification& notification : notifications) {
        if (const auto* saved = std::get_if<grabber::GrabberProjectSaved>(&notification)) {
            const auto pending = pendingSaves_.find(saved->token);
            const AfterSave after = pending == pendingSaves_.end() ? AfterSave::Nothing : pending->second;
            if (pending != pendingSaves_.end()) {
                pendingSaves_.erase(pending);
            }
            if (saved->error) {
                Tell(base::ToUtf16(*saved->error), true);
            } else if (after == AfterSave::OpenWindow && saved->id) {
                OpenWindow(*saved->id);
            }
        } else if (const auto* error = std::get_if<grabber::GrabberError>(&notification)) {
            Tell(base::ToUtf16(error->message), true);
        }
    }
}

bool GrabberController::PreTranslate(MSG& message)
{
    for (const auto& [id, window] : windows_) {
        if (window->Handle() != nullptr && ::IsDialogMessageW(window->Handle(), &message)) {
            return true;
        }
    }
    return false;
}

}  // namespace ldm::ui
