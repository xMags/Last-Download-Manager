#pragma once

#include "ui/Dialog.h"

#include <optional>
#include <string>

namespace ldm::ui {

// The server answered "login required" (checklist O-07): ask for a login, optionally saved for
// the server in Site logins.
class LoginDialog final : public Dialog {
public:
    struct Answer {
        std::string user;
        std::string password;
        bool save = false;
    };

    LoginDialog(const Theme& theme, std::wstring server, std::wstring fileName);
    [[nodiscard]] std::optional<Answer> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;

    std::wstring server_;
    std::wstring fileName_;
    std::optional<Answer> answer_;
};

}  // namespace ldm::ui
