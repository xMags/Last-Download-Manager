#pragma once

#include "model/Ids.h"
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

// The queue panel (checklist A-04), after Download Later, a batch add or Download All Links: which
// queue the new downloads join, and whether it starts now.
class QueuePanelDialog final : public Dialog {
public:
    struct Entry {
        model::QueueId id;
        std::wstring name;
    };
    struct Choice {
        model::QueueId queue;
        bool startNow = false;
        bool dontAskAgain = false;
    };

    // `queues` in their sort order; the first one, the main queue, is chosen at first.
    QueuePanelDialog(const Theme& theme, std::vector<Entry> queues, int downloads);
    // Nothing when the panel was closed: the downloads go into the main queue.
    [[nodiscard]] std::optional<Choice> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;

    std::vector<Entry> queues_;
    int downloads_ = 1;
    std::optional<Choice> choice_;
};

}  // namespace ldm::ui
