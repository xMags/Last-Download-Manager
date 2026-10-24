#pragma once

#include "core/GrabberCommands.h"
#include "core/Snapshot.h"
#include "grabber/GrabberSnapshot.h"
#include "ui/ShellActions.h"
#include "ui/Theme.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ldm::ui {

class GrabberProjectsDialog;
class GrabberWindow;

// The Site Grabber's front end (M4 design section 6.5): the project chooser, the wizard and one
// window per open project. UI thread only. It holds no Grabber state: it reads the explorer's
// newest snapshot and posts commands.
class GrabberController {
public:
    // The answer to a request for a browser's cookies: nothing when no browser answered.
    using CookiesAnswer = std::function<void(std::optional<std::vector<model::Cookie>>)>;

    struct Context {
        HWND owner = nullptr;
        core::GrabberSink* grabber = nullptr;
        ShellActions* shell = nullptr;
        const Theme* theme = nullptr;
        std::function<std::shared_ptr<const core::Snapshot>()> snapshot;
        // Asks a connected browser for the cookies it sends to an address; false when no browser
        // is connected (the answer is then never called).
        std::function<bool(const std::string& url, CookiesAnswer answer)> requestCookies;
    };

    explicit GrabberController(Context context);
    ~GrabberController();
    GrabberController(const GrabberController&) = delete;
    GrabberController& operator=(const GrabberController&) = delete;

    // Tasks > Run site grabber.
    void OpenChooser();
    void OnSnapshot(std::shared_ptr<const grabber::GrabberSnapshot> snapshot);
    void OnNotifications(std::vector<grabber::GrabberNotification> notifications);
    // The main list changed: the windows show their downloads' progress.
    void OnCoreSnapshot(const core::Snapshot& snapshot);
    bool PreTranslate(MSG& message);
    // The theme changed; windows opened after this use it. Open ones keep theirs until reopened.
    void CloseAll();

private:
    // What to do once a saved project's answer comes.
    enum class AfterSave {
        Nothing,
        OpenWindow,
    };

    void NewProject(std::optional<model::GrabberProjectId> fromTemplate);
    void EditProject(model::GrabberProjectId id);
    // Runs the wizard; posts the save when finished.
    void RunWizard(model::GrabberProject project, bool hasPassword, AfterSave after);
    void SaveAsTemplate(model::GrabberProjectId id);
    void OpenWindow(model::GrabberProjectId id);
    void Explore(model::GrabberProjectId id, bool keepChecks);
    void Post(core::GrabberCommand command);
    [[nodiscard]] const grabber::GrabberProjectView* Find(model::GrabberProjectId id) const;
    [[nodiscard]] HWND DialogOwner() const;
    void Tell(const std::wstring& message, bool warning) const;

    Context context_;
    std::shared_ptr<const grabber::GrabberSnapshot> snapshot_;
    std::map<std::int64_t, std::unique_ptr<GrabberWindow>> windows_;  // by project id
    // Windows that were destroyed, kept until no call of theirs is on the stack any more.
    std::vector<std::unique_ptr<GrabberWindow>> closing_;
    GrabberProjectsDialog* chooser_ = nullptr;  // while the chooser is open
    std::map<std::uint64_t, AfterSave> pendingSaves_;  // by token
    std::uint64_t nextToken_ = 1;
};

}  // namespace ldm::ui
