#pragma once

#include "ui/Dialog.h"

#include <optional>
#include <string>

namespace ldm::ui {

// Options > Site videos > Sign in on sites (video-sites round 2, decision E1): the sites whose
// pages LDM asks about with the browser's cookies for them, so videos that need the user's
// account work. Each site is the user's own choice; YouTube carries a warning.
class SiteSignInDialog final : public Dialog {
public:
    // `setting`: Settings::siteVideosSignIn.
    SiteSignInDialog(const Theme& theme, std::string setting);

    // The new setting; nothing when cancelled.
    [[nodiscard]] std::optional<std::string> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;

    std::string setting_;
    std::optional<std::string> result_;
};

// "Vimeo, Instagram" for the sites `setting` turns on, "(none)" without any.
[[nodiscard]] std::wstring SignInSummary(const std::string& setting);

}  // namespace ldm::ui
