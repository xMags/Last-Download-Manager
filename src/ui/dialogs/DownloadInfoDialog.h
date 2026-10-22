#pragma once

#include "core/Snapshot.h"
#include "model/Category.h"
#include "ui/Dialog.h"
#include "ui/Handles.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ldm::ui {

// Download File Info (checklist A-02, A-03): where and under which name to save a new download.
// With speculative start the transfer already runs while this is open; Update() feeds it the
// server's name, size and type as they arrive, without overwriting what the user typed.
class DownloadInfoDialog final : public Dialog {
public:
    enum class Action {
        Start,
        Later,
        Cancel,
    };

    struct Choice {
        Action action = Action::Cancel;
        std::string fileName;
        std::string saveDir;
        std::optional<model::CategoryId> category;
        std::string description;
        bool rememberFolder = false;
    };

    struct Initial {
        std::string url;
        std::string fileName;
        std::string saveDir;
        std::optional<model::CategoryId> category;
        std::vector<model::Category> categories;
        std::vector<std::string> recentFolders;
    };

    // `addCategory` runs the category dialog and returns the new category, if one was added.
    DownloadInfoDialog(const Theme& theme, Initial initial,
                       std::function<std::optional<model::Category>(HWND)> addCategory);

    [[nodiscard]] Choice Run(HWND owner);
    // Live values of the download being added (speculative start). Safe to call while running.
    void Update(const core::DownloadView& view);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;
    void FillCategories();
    void OnCategoryChanged();
    void Browse();
    bool Finish(Action action);
    [[nodiscard]] std::optional<model::CategoryId> SelectedCategory() const;

    Initial initial_;
    std::function<std::optional<model::Category>(HWND)> addCategory_;
    Choice choice_;
    bool nameEdited_ = false;
    bool folderEdited_ = false;
    bool settingText_ = false;
    UniqueIcon fileIcon_;
};

}  // namespace ldm::ui
