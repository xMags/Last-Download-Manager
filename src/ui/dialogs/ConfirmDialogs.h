#pragma once

#include "model/Settings.h"
#include "ui/Dialog.h"

#include <optional>

namespace ldm::ui {

// Remove confirmation (checklist L-07). With completed downloads selected it offers to delete
// their files too; partial files of unfinished downloads are always removed with the entry.
class DeleteDialog final : public Dialog {
public:
    struct Answer {
        bool deleteFiles = false;
        bool dontAskAgain = false;
    };

    DeleteDialog(const Theme& theme, size_t count, bool anyCompleted, bool anyUnfinished);
    [[nodiscard]] std::optional<Answer> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;

    size_t count_;
    bool anyCompleted_;
    bool anyUnfinished_;
    std::optional<Answer> answer_;
};

// The address is already in the list and Options say to ask (checklist A-05).
class DuplicateDialog final : public Dialog {
public:
    struct Answer {
        model::DuplicatePolicy policy = model::DuplicatePolicy::AddNumbered;
        bool remember = false;
    };

    DuplicateDialog(const Theme& theme, std::wstring fileName, bool existingComplete);
    [[nodiscard]] std::optional<Answer> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;

    std::wstring fileName_;
    bool existingComplete_;
    std::optional<Answer> answer_;
};

}  // namespace ldm::ui
