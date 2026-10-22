#pragma once

#include "ui/BatchPattern.h"
#include "ui/Dialog.h"

#include <optional>
#include <string>
#include <vector>

namespace ldm::ui {

// Tasks > Add batch download (checklist A-07), with a live preview of the first, second and last
// address.
class BatchDialog final : public Dialog {
public:
    struct Choice {
        std::vector<std::string> urls;
        std::optional<std::string> user;
        std::optional<std::string> password;
        bool start = false;
    };

    explicit BatchDialog(const Theme& theme);
    [[nodiscard]] std::optional<Choice> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;
    [[nodiscard]] BatchPattern ReadPattern() const;
    void UpdatePreview();
    void UpdateEnabled();

    std::optional<Choice> result_;
    bool initializing_ = true;
};

}  // namespace ldm::ui
