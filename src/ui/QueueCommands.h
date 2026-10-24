#pragma once

#include "core/Commands.h"
#include "core/Snapshot.h"
#include "model/Ids.h"
#include "ui/Theme.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ldm::ui {

// The queue entries of the menus, the toolbar and the tree (M4 design section 7): Add to queue
// and Delete from queue on rows, Start queue and Stop queue, and the tree's queue menu. Menus list
// the queues of the newest snapshot; the queue each item stands for is remembered when the menu is
// built, so a snapshot arriving while the menu is open changes nothing.
class QueueCommands {
public:
    struct Context {
        HWND owner = nullptr;
        core::CommandSink* commands = nullptr;
        const Theme* theme = nullptr;
        std::function<std::shared_ptr<const core::Snapshot>()> snapshot;
        std::function<std::vector<const core::DownloadView*>()> selection;
        std::function<void()> openScheduler;
    };

    explicit QueueCommands(Context context);

    // "Add to queue" with the queues and "Create new queue", then "Delete from queue".
    void AppendRowItems(HMENU menu, std::span<const core::DownloadView* const> selection);
    // Fills the Downloads menu's Start queue or Stop queue popup when `menu` is one of them.
    // Returns false for any other menu.
    bool FillPopup(HMENU menu);
    // The toolbar's Start Queue and Stop Queue drop-downs, under the button.
    void ShowToolbarMenu(int command, POINT screen);
    // Right-click on the tree's Queues node (no queue) or on one queue.
    void ShowTreeMenu(std::optional<model::QueueId> queue, POINT screen);
    // --start-queue (checklist A-10): the main queue.
    void StartMainQueue();

    // Returns true when `id` is one of these commands.
    bool OnCommand(int id);

private:
    enum class Kind {
        Start,
        Stop,
    };
    void FillQueueList(HMENU menu, Kind kind);
    void AddSelectionTo(model::QueueId queue);
    // Asks for a name and adds the queue with `members` (none from the tree's New queue).
    void CreateQueueFor(std::span<const core::DownloadView* const> members);
    // M4 design decision 3: whether completed downloads join an ordinary queue, where they are
    // downloaded again. `others`: some downloads that are not complete join anyway. Nothing when
    // the user cancelled.
    [[nodiscard]] std::optional<bool> AskAboutCompleted(int completed, bool others) const;
    void DeleteQueue(model::QueueId queue);
    void Post(core::Command command);
    [[nodiscard]] bool Ask(const std::wstring& question) const;

    Context context_;
    // The queue of each item of the menus built last, by item offset from the range's first id.
    std::vector<model::QueueId> addTargets_;
    std::vector<model::QueueId> startTargets_;
    std::vector<model::QueueId> stopTargets_;
    std::optional<model::QueueId> treeTarget_;
};

}  // namespace ldm::ui
