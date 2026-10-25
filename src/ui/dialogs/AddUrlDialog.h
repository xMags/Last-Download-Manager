#pragma once

#include "ui/Dialog.h"

#include <optional>
#include <string>
#include <vector>

namespace ldm::ui {

struct AddUrlInput {
    std::string url;
    std::optional<std::string> user;
    std::optional<std::string> password;
    bool findVideo = false;  // "Find video on the page": the address is a page, not a file
};

// Tasks > Add new download (checklist A-01): an address with history and optional credentials.
// Only http, https, ftp and ftps addresses are accepted. With site videos on, a page's address
// may be given to find the video on it instead.
class AddUrlDialog final : public Dialog {
public:
    AddUrlDialog(const Theme& theme, std::vector<std::string> history, std::string initialUrl, bool siteVideos);

    [[nodiscard]] std::optional<AddUrlInput> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;
    void UpdateAuthControls();

    std::vector<std::string> history_;
    std::string initialUrl_;
    bool siteVideos_ = false;
    std::optional<AddUrlInput> result_;
};

}  // namespace ldm::ui
