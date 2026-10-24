#pragma once

#include "model/Category.h"
#include "model/GrabberProject.h"
#include "ui/Dialog.h"

#include <optional>
#include <string>
#include <vector>

namespace ldm::ui {

// The Site Grabber's project wizard (checklist G-01 to G-04, M4 design section 6.5): the project,
// where to save, which pages to explore and which files to list, on four pages switched like
// Options' pages. Modal. Every field is checked before Finish, as the explorer checks it again.
class GrabberWizard final : public Dialog {
public:
    struct Result {
        model::GrabberProject project;
        // The login password typed: nothing keeps the stored one, empty clears it.
        std::optional<std::string> password;
    };

    // `takenNames`: other projects' and templates' names, refused here ignoring case.
    GrabberWizard(const Theme& theme, model::GrabberProject project, bool hasPassword,
                  std::vector<model::Category> categories, std::vector<std::wstring> takenNames);
    [[nodiscard]] std::optional<Result> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;

    void ShowPage(int page);
    void LoadFields(const model::GrabberProjectDefinition& definition);
    void EnableFields();
    // Reads one page into `project_`; false when a field is not valid, which a strict read shows
    // the user on that page. A page that is not valid may be read only in part.
    bool ReadPage(int page, bool strict);
    void ApplyChosenTemplate();
    [[nodiscard]] model::GrabberTemplate ChosenTemplate() const;

    model::GrabberProject project_;
    bool hasPassword_ = false;
    bool passwordEdited_ = false;
    std::vector<model::Category> categories_;
    std::vector<std::wstring> taken_;
    int page_ = 0;
    bool loading_ = false;
    std::optional<Result> result_;
};

}  // namespace ldm::ui
