#pragma once

#include "grabber/GrabberSnapshot.h"
#include "model/Ids.h"
#include "ui/Dialog.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ldm::ui {

// Tasks > Run site grabber (checklist G-07, MN-01): the projects and templates, most recently run
// first, with what can be done to them. Modal; Rename and Delete act at once and the list follows
// the explorer's snapshots, the other buttons close it with the choice made.
class GrabberProjectsDialog final : public Dialog {
public:
    enum class Action {
        NewProject,
        NewFromTemplate,
        Open,
        Edit,
    };
    struct Choice {
        Action action = Action::NewProject;
        std::optional<model::GrabberProjectId> project;
    };
    struct Callbacks {
        std::function<void(model::GrabberProjectId, std::string)> rename;
        std::function<void(model::GrabberProjectId)> remove;
    };

    GrabberProjectsDialog(const Theme& theme, std::shared_ptr<const grabber::GrabberSnapshot> snapshot,
                          Callbacks callbacks);
    [[nodiscard]] std::optional<Choice> Run(HWND owner);
    void Update(std::shared_ptr<const grabber::GrabberSnapshot> snapshot);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;
    bool OnNotify(const NMHDR& header, LRESULT& result) override;

    void Refresh();
    void RefreshButtons();
    [[nodiscard]] const grabber::GrabberProjectView* Selected() const;
    void Choose(Action action);

    std::shared_ptr<const grabber::GrabberSnapshot> snapshot_;
    Callbacks callbacks_;
    std::vector<model::GrabberProjectId> rows_;
    std::optional<model::GrabberProjectId> selectAfterRefresh_;
    std::optional<Choice> choice_;
};

// Asks for a project's or a template's name (Rename, Save as template). Names in `taken` are
// refused here, ignoring case, as the explorer would refuse them.
class GrabberNameDialog final : public Dialog {
public:
    GrabberNameDialog(const Theme& theme, std::wstring title, std::wstring initial, std::vector<std::wstring> taken);
    [[nodiscard]] std::optional<std::string> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;

    std::wstring title_;
    std::wstring initial_;
    std::vector<std::wstring> taken_;
    std::optional<std::string> name_;
};

// The names of every project and template but `except`, for the name checks.
[[nodiscard]] std::vector<std::wstring> GrabberNames(const grabber::GrabberSnapshot* snapshot,
                                                     std::optional<model::GrabberProjectId> except);

}  // namespace ldm::ui
