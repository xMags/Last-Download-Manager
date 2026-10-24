#pragma once

#include "ui/Dialog.h"

#include <optional>
#include <string>
#include <vector>

namespace ldm::ui {

// Asks for the name of a new download queue (Add to queue > Create new queue, the Scheduler's
// New queue). Names already taken are refused here, ignoring case, as the core would refuse them.
class QueueNameDialog final : public Dialog {
public:
    QueueNameDialog(const Theme& theme, std::vector<std::wstring> takenNames);
    // The trimmed name, in UTF-8; nothing when cancelled.
    [[nodiscard]] std::optional<std::string> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;

    std::vector<std::wstring> taken_;
    std::optional<std::string> name_;
};

}  // namespace ldm::ui
