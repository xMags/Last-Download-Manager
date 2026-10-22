#pragma once

#include "core/Commands.h"
#include "core/Snapshot.h"
#include "ui/Dialog.h"
#include "ui/Handles.h"

#include <functional>
#include <optional>

namespace ldm::ui {

// Download properties (checklist C-02). Editing the address, description, referer and login is
// allowed; the changes are returned as one update command holding only the fields that changed.
class PropertiesDialog final : public Dialog {
public:
    struct Callbacks {
        std::function<void(HWND)> open;  // Open button
        std::function<void(HWND)> move;  // Move... button
    };

    PropertiesDialog(const Theme& theme, core::DownloadView view, Callbacks callbacks);
    [[nodiscard]] std::optional<core::UpdateDownloadCommand> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;

    core::DownloadView view_;
    Callbacks callbacks_;
    std::optional<core::UpdateDownloadCommand> result_;
    UniqueIcon icon_;
};

// Move/Rename (checklist L-05): a new folder and file name for a download.
class MoveDialog final : public Dialog {
public:
    struct Choice {
        std::wstring folder;
        std::wstring fileName;
    };

    MoveDialog(const Theme& theme, std::wstring folder, std::wstring fileName, std::vector<std::string> recentFolders);
    [[nodiscard]] std::optional<Choice> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;

    std::wstring folder_;
    std::wstring fileName_;
    std::vector<std::string> recentFolders_;
    std::optional<Choice> result_;
};

}  // namespace ldm::ui
