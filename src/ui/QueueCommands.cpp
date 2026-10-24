#include "ui/QueueCommands.h"

#include "base/Utf.h"
#include "resource.h"
#include "ui/Handles.h"
#include "ui/QueueChoice.h"
#include "ui/dialogs/MessageDialog.h"
#include "ui/dialogs/QueueDialogs.h"

#include <algorithm>
#include <string>

namespace ldm::ui {

namespace {

constexpr size_t kMenuQueues = ID_ADD_TO_QUEUE_LAST - ID_ADD_TO_QUEUE_FIRST + 1;
static_assert(ID_START_QUEUE_LAST - ID_START_QUEUE_FIRST + 1 == kMenuQueues);
static_assert(ID_STOP_QUEUE_LAST - ID_STOP_QUEUE_FIRST + 1 == kMenuQueues);

// A queue name as menu text: '&' would underline the next letter.
std::wstring MenuText(const std::string& name)
{
    std::wstring text;
    for (const wchar_t c : base::ToUtf16(name)) {
        text += c;
        if (c == L'&') {
            text += L'&';
        }
    }
    return text;
}

void AppendItem(HMENU menu, UINT id, const std::wstring& text, bool enabled = true)
{
    ::AppendMenuW(menu, MF_STRING | (enabled ? MF_ENABLED : MF_GRAYED), id, text.c_str());
}

void AppendSeparator(HMENU menu)
{
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
}

bool InRange(UINT id, UINT first, UINT last)
{
    return id >= first && id <= last;
}

std::optional<model::QueueId> Target(const std::vector<model::QueueId>& targets, int id, int first)
{
    const auto offset = static_cast<size_t>(id - first);
    return offset < targets.size() ? std::optional<model::QueueId>(targets[offset]) : std::nullopt;
}

}  // namespace

QueueCommands::QueueCommands(Context context) : context_(std::move(context)) {}

void QueueCommands::Post(core::Command command)
{
    context_.commands->Post(std::move(command));
}

bool QueueCommands::Ask(const std::wstring& question) const
{
    return ShowMessage(context_.owner, *context_.theme,
                       MessageRequest{question, MessageKind::Question, MessageButtons::YesNo}) == IDYES;
}

void QueueCommands::AppendRowItems(HMENU menu, std::span<const core::DownloadView* const> selection)
{
    const auto snapshot = context_.snapshot();
    if (!snapshot) {
        return;
    }
    const bool any = std::any_of(selection.begin(), selection.end(), [](const core::DownloadView* view) {
        return view->download.status != model::DownloadStatus::Deleting;
    });
    UniqueMenu queues(::CreatePopupMenu());
    addTargets_.clear();
    for (const core::QueueView& view : snapshot->queues) {
        if (addTargets_.size() == kMenuQueues) {
            break;
        }
        // A queue every selected download is in already has nothing to add.
        const bool allIn = std::all_of(selection.begin(), selection.end(), [&view](const core::DownloadView* download) {
            return download->download.queueId == view.queue.id;
        });
        AppendItem(queues.get(), static_cast<UINT>(ID_ADD_TO_QUEUE_FIRST + addTargets_.size()), MenuText(view.queue.name),
                   any && !allIn);
        addTargets_.push_back(view.queue.id);
    }
    AppendSeparator(queues.get());
    AppendItem(queues.get(), ID_ROW_ADD_TO_NEW_QUEUE, L"&Create new queue...", any);
    ::AppendMenuW(menu, MF_POPUP | (any ? MF_ENABLED : MF_GRAYED), reinterpret_cast<UINT_PTR>(queues.release()),
                  L"Add to &queue");  // owned by `menu` now
    const bool inQueue = std::any_of(selection.begin(), selection.end(),
                                     [](const core::DownloadView* view) { return view->download.queueId.has_value(); });
    AppendItem(menu, ID_ROW_DELETE_FROM_QUEUE, L"Delete from q&ueue", inQueue);
}

bool QueueCommands::FillPopup(HMENU menu)
{
    if (::GetMenuItemCount(menu) <= 0) {
        return false;
    }
    const UINT first = ::GetMenuItemID(menu, 0);
    if (first == ID_DOWNLOADS_START_QUEUE || InRange(first, ID_START_QUEUE_FIRST, ID_START_QUEUE_LAST)) {
        FillQueueList(menu, Kind::Start);
        return true;
    }
    if (first == ID_DOWNLOADS_STOP_QUEUE || InRange(first, ID_STOP_QUEUE_FIRST, ID_STOP_QUEUE_LAST)) {
        FillQueueList(menu, Kind::Stop);
        return true;
    }
    return false;
}

void QueueCommands::FillQueueList(HMENU menu, Kind kind)
{
    while (::GetMenuItemCount(menu) > 0) {
        ::DeleteMenu(menu, 0, MF_BYPOSITION);
    }
    std::vector<model::QueueId>& targets = kind == Kind::Start ? startTargets_ : stopTargets_;
    const UINT first = kind == Kind::Start ? ID_START_QUEUE_FIRST : ID_STOP_QUEUE_FIRST;
    targets.clear();
    const auto snapshot = context_.snapshot();
    if (snapshot) {
        for (const core::QueueView& view : snapshot->queues) {
            if (targets.size() == kMenuQueues) {
                break;
            }
            // Start lists the queues that are stopped, Stop those that run; the others are grayed.
            const bool applies = kind == Kind::Start ? !view.running : view.running;
            AppendItem(menu, static_cast<UINT>(first + targets.size()), MenuText(view.queue.name), applies);
            targets.push_back(view.queue.id);
        }
    }
    if (targets.empty()) {
        AppendItem(menu, kind == Kind::Start ? ID_DOWNLOADS_START_QUEUE : ID_DOWNLOADS_STOP_QUEUE, L"(no queues)", false);
    }
}

void QueueCommands::ShowToolbarMenu(int command, POINT screen)
{
    UniqueMenu menu(::CreatePopupMenu());
    AppendItem(menu.get(), command == ID_DOWNLOADS_START_QUEUE ? ID_DOWNLOADS_START_QUEUE : ID_DOWNLOADS_STOP_QUEUE, L"");
    FillQueueList(menu.get(), command == ID_DOWNLOADS_START_QUEUE ? Kind::Start : Kind::Stop);
    ::TrackPopupMenuEx(menu.get(), TPM_RIGHTBUTTON, screen.x, screen.y, context_.owner, nullptr);
}

void QueueCommands::ShowTreeMenu(std::optional<model::QueueId> queue, POINT screen)
{
    const auto snapshot = context_.snapshot();
    const core::QueueView* view = snapshot && queue ? FindQueueView(*snapshot, *queue) : nullptr;
    treeTarget_ = view != nullptr ? std::optional<model::QueueId>(view->queue.id) : std::nullopt;
    UniqueMenu menu(::CreatePopupMenu());
    if (view != nullptr) {
        AppendItem(menu.get(), ID_QUEUE_START, L"&Start queue", !view->running);
        AppendItem(menu.get(), ID_QUEUE_STOP, L"S&top queue", view->running);
        AppendSeparator(menu.get());
    }
    AppendItem(menu.get(), ID_QUEUE_CREATE, L"&New queue...");
    if (view != nullptr) {
        AppendItem(menu.get(), ID_QUEUE_DELETE, L"&Delete queue", !view->queue.IsBuiltin());
    }
    AppendSeparator(menu.get());
    AppendItem(menu.get(), ID_QUEUE_SCHEDULER, L"Sc&heduler...", static_cast<bool>(context_.openScheduler));
    ::TrackPopupMenuEx(menu.get(), TPM_RIGHTBUTTON, screen.x, screen.y, context_.owner, nullptr);
}

void QueueCommands::StartMainQueue()
{
    const auto snapshot = context_.snapshot();
    if (const auto main = snapshot ? MainQueueId(*snapshot) : std::nullopt) {
        Post(core::StartQueueCommand{*main});
    }
}

bool QueueCommands::OnCommand(int id)
{
    if (InRange(static_cast<UINT>(id), ID_ADD_TO_QUEUE_FIRST, ID_ADD_TO_QUEUE_LAST)) {
        if (const auto queue = Target(addTargets_, id, ID_ADD_TO_QUEUE_FIRST)) {
            AddSelectionTo(*queue);
        }
        return true;
    }
    if (InRange(static_cast<UINT>(id), ID_START_QUEUE_FIRST, ID_START_QUEUE_LAST)) {
        if (const auto queue = Target(startTargets_, id, ID_START_QUEUE_FIRST)) {
            Post(core::StartQueueCommand{*queue});
        }
        return true;
    }
    if (InRange(static_cast<UINT>(id), ID_STOP_QUEUE_FIRST, ID_STOP_QUEUE_LAST)) {
        if (const auto queue = Target(stopTargets_, id, ID_STOP_QUEUE_FIRST)) {
            Post(core::StopQueueCommand{*queue});
        }
        return true;
    }
    switch (id) {
    case ID_ROW_ADD_TO_NEW_QUEUE:
        CreateQueueFor(context_.selection());
        return true;
    case ID_ROW_DELETE_FROM_QUEUE: {
        std::vector<model::DownloadId> ids;
        for (const core::DownloadView* view : context_.selection()) {
            if (view->download.queueId) {
                ids.push_back(view->download.id);
            }
        }
        if (!ids.empty()) {
            Post(core::SetQueueCommand{std::move(ids), std::nullopt});
        }
        return true;
    }
    case ID_QUEUE_START:
        if (treeTarget_) {
            Post(core::StartQueueCommand{*treeTarget_});
        }
        return true;
    case ID_QUEUE_STOP:
        if (treeTarget_) {
            Post(core::StopQueueCommand{*treeTarget_});
        }
        return true;
    case ID_QUEUE_CREATE:
        CreateQueueFor({});
        return true;
    case ID_QUEUE_DELETE:
        if (treeTarget_) {
            DeleteQueue(*treeTarget_);
        }
        return true;
    case ID_DOWNLOADS_SCHEDULER:
        if (context_.openScheduler) {
            context_.openScheduler(std::nullopt);
        }
        return true;
    case ID_QUEUE_SCHEDULER:
        if (context_.openScheduler) {
            context_.openScheduler(treeTarget_);
        }
        return true;
    default:
        return false;
    }
}

std::optional<bool> QueueCommands::AskAboutCompleted(int completed, bool others) const
{
    // M4 design decision 3: a completed download in a queue is downloaded again.
    const bool one = completed == 1;
    std::wstring question = others ? (one ? std::wstring(L"One of these downloads is already complete.")
                                          : std::to_wstring(completed) + L" of these downloads are already complete.")
                                   : (one ? L"This download is already complete." : L"These downloads are already complete.");
    question += std::wstring(L" Download ") + (one ? L"it" : L"them") + L" again when the queue runs? The existing " +
                (one ? L"file is" : L"files are") + L" replaced when the new copy is complete.";
    if (others) {
        question += L"\n\nChoose No to add only the downloads that are not complete.";
    }
    const int answer = ShowMessage(context_.owner, *context_.theme,
                                   MessageRequest{question, MessageKind::Question,
                                                  others ? MessageButtons::YesNoCancel : MessageButtons::YesNo});
    if (answer == IDCANCEL || (answer == IDNO && !others)) {
        return std::nullopt;
    }
    return answer == IDYES;
}

void QueueCommands::AddSelectionTo(model::QueueId queue)
{
    const auto snapshot = context_.snapshot();
    const core::QueueView* target = snapshot ? FindQueueView(*snapshot, queue) : nullptr;
    if (target == nullptr) {
        return;
    }
    const std::vector<const core::DownloadView*> selection = context_.selection();
    bool includeCompleted = true;
    if (const int completed = CompletedJoiners(selection, target->queue); completed > 0) {
        const auto answer = AskAboutCompleted(completed, !QueueJoiners(selection, target->queue, false).empty());
        if (!answer) {
            return;
        }
        includeCompleted = *answer;
    }
    std::vector<model::DownloadId> ids = QueueJoiners(selection, target->queue, includeCompleted);
    if (!ids.empty()) {
        Post(core::SetQueueCommand{std::move(ids), queue});
    }
}

void QueueCommands::CreateQueueFor(std::span<const core::DownloadView* const> members)
{
    const auto snapshot = context_.snapshot();
    std::vector<std::wstring> taken;
    if (snapshot) {
        for (const core::QueueView& view : snapshot->queues) {
            taken.push_back(base::ToUtf16(view.queue.name));
        }
    }
    // A new queue is an ordinary queue: completed downloads are asked about as for any other.
    model::Queue queue;
    bool includeCompleted = true;
    if (const int completed = CompletedJoiners(members, queue); completed > 0) {
        const auto answer = AskAboutCompleted(completed, !QueueJoiners(members, queue, false).empty());
        if (!answer) {
            return;
        }
        includeCompleted = *answer;
    }
    QueueNameDialog dialog(*context_.theme, std::move(taken));
    const auto name = dialog.Run(context_.owner);
    if (!name) {
        return;
    }
    queue.name = *name;
    Post(core::SaveQueueCommand{std::move(queue), QueueJoiners(members, model::Queue{}, includeCompleted)});
}

void QueueCommands::DeleteQueue(model::QueueId queue)
{
    const auto snapshot = context_.snapshot();
    const core::QueueView* view = snapshot ? FindQueueView(*snapshot, queue) : nullptr;
    if (view == nullptr || view->queue.IsBuiltin()) {
        return;
    }
    if (Ask(L"Delete the queue \"" + base::ToUtf16(view->queue.name) +
            L"\"? Its downloads stay in the list, outside any queue.")) {
        Post(core::DeleteQueueCommand{queue});
    }
}

}  // namespace ldm::ui
