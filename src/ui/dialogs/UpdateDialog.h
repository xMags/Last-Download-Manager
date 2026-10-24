#pragma once

#include "ui/Dialog.h"

#include <string>

namespace ldm::ui {

// A newer version of LDM is on GitHub (checklist X-05): open its release page, skip it, or decide
// later. Nothing is downloaded or installed.
class UpdateDialog final : public Dialog {
public:
    enum class Choice {
        OpenPage,
        Skip,
        Later,
    };

    UpdateDialog(const Theme& theme, std::wstring version);
    [[nodiscard]] Choice Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;

    std::wstring version_;
    Choice choice_ = Choice::Later;
};

}  // namespace ldm::ui
