#include "ui/MainController.h"

#include "base/Utf.h"
#include "resource.h"
#include "ui/CommandState.h"
#include "ui/Format.h"
#include "ui/History.h"
#include "ui/StopChoice.h"
#include "ui/dialogs/CategoryDialog.h"
#include "ui/dialogs/ConfirmDialogs.h"
#include "core/Url.h"
#include "ui/dialogs/ListDialogs.h"
#include "ui/dialogs/LoginDialog.h"
#include "ui/dialogs/MessageDialog.h"
#include "ui/dialogs/OptionsDialog.h"
#include "ui/dialogs/PropertiesDialog.h"
#include "ui/dialogs/SchedulerWindow.h"

#include <commdlg.h>

#include <algorithm>
#include <filesystem>

namespace ldm::ui {

namespace {

constexpr wchar_t kHelpUrl[] = L"https://github.com/xMags/Last-Download-Manager#readme";

std::filesystem::path FinalPath(const model::Download& download)
{
    return std::filesystem::path(base::ToUtf16(download.saveDir)) / base::ToUtf16(download.fileName);
}

void AppendItem(HMENU menu, UINT id, const wchar_t* text, bool enabled = true, bool checked = false)
{
    ::AppendMenuW(menu, MF_STRING | (enabled ? MF_ENABLED : MF_GRAYED) | (checked ? MF_CHECKED : MF_UNCHECKED), id, text);
}

void AppendSeparator(HMENU menu)
{
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
}

}  // namespace

MainController::MainController(Context context)
    : context_(std::move(context)),
      transfers_(TransferWindows::Context{context_.owner, context_.commands, context_.shell, context_.system,
                                          context_.theme, context_.snapshot, context_.exit}),
      captures_(CaptureFollowUp::Context{context_.owner, context_.commands, context_.theme, context_.snapshot,
                                         context_.openPage}),
      adds_(AddFlow::Context{context_.owner, context_.commands, context_.theme, context_.snapshot,
                             [this](model::DownloadId id) { transfers_.OpenProgress(id); },
                             [this](model::DownloadId id) { context_.list->SelectOnly(id); }, context_.linksDone,
                             [this](model::DownloadId id) { captures_.OnCapturedAdded(id); },
                             [this](const std::string& url) { captures_.OnCaptureCancelled(url); }}),
      importExport_(ImportExportFlow::Context{context_.owner, context_.theme, &adds_, context_.snapshot,
                                              [this] { return Selection(); },
                                              [this]() -> std::optional<model::QueueId> {
                                                  const ListFilter filter = context_.currentFilter();
                                                  return filter.status == StatusFilter::Queued ? filter.queue : std::nullopt;
                                              }}),
      playlists_(PlaylistOffer::Context{context_.owner, context_.theme,
                                        [this](const std::vector<media::M3uEntry>& entries, const std::string& url) {
                                            adds_.AddPlaylistLinks(entries, url);
                                        }}),
      queues_(QueueCommands::Context{context_.owner, context_.commands, context_.theme, context_.snapshot,
                                     [this] { return Selection(); },
                                     [this](std::optional<model::QueueId> queue) { OpenScheduler(queue); }})
{
}

MainController::~MainController() = default;

std::shared_ptr<const core::Snapshot> MainController::Snapshot() const
{
    return context_.snapshot();
}

std::vector<const core::DownloadView*> MainController::Selection() const
{
    return context_.list->Selection();
}

const core::DownloadView* MainController::SingleSelection() const
{
    const auto selection = Selection();
    return selection.size() == 1 ? selection.front() : nullptr;
}

void MainController::Post(core::Command command)
{
    context_.commands->Post(std::move(command));
}

void MainController::ModifySettings(std::function<void(model::Settings&)> modify)
{
    Post(core::ModifySettingsCommand{std::move(modify)});
}

void MainController::ShowError(const std::wstring& message) const
{
    (void)ShowMessage(context_.owner, *context_.theme, MessageRequest{message, MessageKind::Warning});
}

bool MainController::Ask(const std::wstring& question) const
{
    return ShowMessage(context_.owner, *context_.theme, MessageRequest{question, MessageKind::Question, MessageButtons::YesNo}) ==
           IDYES;
}

void MainController::Tell(const std::wstring& message) const
{
    (void)ShowMessage(context_.owner, *context_.theme, MessageRequest{message, MessageKind::Information});
}

bool MainController::PreTranslate(MSG& message)
{
    if (scheduler_ && scheduler_->Handle() != nullptr && ::IsDialogMessageW(scheduler_->Handle(), &message)) {
        return true;
    }
    if (find_ && find_->Handle() != nullptr && ::IsDialogMessageW(find_->Handle(), &message)) {
        return true;
    }
    return transfers_.PreTranslate(message);
}

bool MainController::OnCommand(int id)
{
    if (queues_.OnCommand(id)) {
        return true;
    }
    const auto selectedIds = [this] { return context_.list->SelectedIds(); };
    switch (id) {
    case ID_TASKS_ADD_URL:
        adds_.AddUrl({});
        return true;
    case ID_TASKS_ADD_BATCH:
        adds_.AddBatch();
        return true;
    case ID_TASKS_ADD_BATCH_CLIPBOARD:
        adds_.AddFromClipboard();
        return true;
    case ID_TASKS_DROP_TARGET:
        ModifySettings([](model::Settings& s) { s.showDropTarget = !s.showDropTarget; });
        return true;
    case ID_TASKS_EXPORT_LDM:
        importExport_.ExportLdmFile();
        return true;
    case ID_TASKS_EXPORT_TEXT:
        importExport_.ExportTextFile();
        return true;
    case ID_TASKS_IMPORT_LDM:
        importExport_.ImportLdmFile();
        return true;
    case ID_TASKS_IMPORT_TEXT:
        importExport_.ImportTextFile();
        return true;
    case ID_TASKS_EXIT:
        context_.exit();
        return true;
    case ID_TRAY_RESTORE:
        context_.restore();
        return true;
    case ID_FILE_DOWNLOAD_NOW: {
        const auto ids = selectedIds();
        Post(core::ResumeCommand{ids});
        // One window per started download, but never a flood of them.
        if (ids.size() <= 3) {
            for (const model::DownloadId started : ids) {
                transfers_.OpenProgress(started);
            }
        }
        return true;
    }
    case ID_FILE_STOP: {
        const auto ask = [this](const std::wstring& question) {
            return AskRecordingStop(context_.owner, *context_.theme, question);
        };
        for (core::Command& command : StopCommands(Selection(), ask)) {
            Post(std::move(command));
        }
        return true;
    }
    case ID_FILE_REMOVE:
        RemoveSelected();
        return true;
    case ID_ROW_REFRESH_ADDRESS:
        if (const auto* view = SingleSelection()) {
            captures_.RefreshAddress(view->download.id);
        }
        return true;
    case ID_FILE_REDOWNLOAD:
        if (const auto* view = SingleSelection()) {
            const bool complete = view->download.status == model::DownloadStatus::Completed;
            if (!complete || Ask(L"Download this file again? The existing file is replaced when the new copy is complete.")) {
                Post(core::RedownloadCommand{view->download.id});
            }
        }
        return true;
    case ID_DOWNLOADS_PAUSE_ALL:
    case ID_DOWNLOADS_STOP_ALL:
        Post(core::StopAllCommand{});
        return true;
    case ID_DOWNLOADS_DELETE_COMPLETED:
        if (Ask(L"Remove every completed download from the list? The files stay on disk.")) {
            Post(core::DeleteCompletedCommand{});
        }
        return true;
    case ID_DOWNLOADS_FIND:
        Find(true);
        return true;
    case ID_DOWNLOADS_FIND_NEXT:
        Find(false);
        return true;
    case ID_DOWNLOADS_LIMITER_ON:
        Post(core::SetSpeedLimiterCommand{true, std::nullopt});
        return true;
    case ID_DOWNLOADS_LIMITER_OFF:
        Post(core::SetSpeedLimiterCommand{false, std::nullopt});
        return true;
    case ID_DOWNLOADS_LIMITER_SETTINGS:
        SpeedLimiterSettings();
        return true;
    case ID_DOWNLOADS_OPTIONS:
        ShowOptions();
        return true;
    case ID_VIEW_HIDE_CATEGORIES:
        ModifySettings([](model::Settings& s) { s.hideCategories = !s.hideCategories; });
        return true;
    case ID_VIEW_TOOLBAR_CUSTOMIZE:
        context_.toolbar->Customize();
        return true;
    case ID_VIEW_TOOLBAR_LARGE:
        ModifySettings([](model::Settings& s) { s.largeToolbarButtons = true; });
        return true;
    case ID_VIEW_TOOLBAR_SMALL:
        ModifySettings([](model::Settings& s) { s.largeToolbarButtons = false; });
        return true;
    case ID_VIEW_TRAY_COLOR:
        ModifySettings([](model::Settings& s) { s.trayIconStyle = model::TrayIconStyle::Color; });
        return true;
    case ID_VIEW_TRAY_CLASSIC:
        ModifySettings([](model::Settings& s) { s.trayIconStyle = model::TrayIconStyle::Classic; });
        return true;
    case ID_VIEW_TRAY_HIDDEN:
        ModifySettings([](model::Settings& s) { s.trayIconStyle = model::TrayIconStyle::Hidden; });
        return true;
    case ID_VIEW_COLUMNS:
        CustomizeColumns();
        return true;
    case ID_VIEW_DARK_MODE:
        ModifySettings([](model::Settings& s) { s.darkModeSupport = !s.darkModeSupport; });
        return true;
    case ID_VIEW_FONT_CHOOSE:
        ChooseListFont();
        return true;
    case ID_VIEW_FONT_RESET:
        ModifySettings([](model::Settings& s) { s.listFont.clear(); });
        Tell(L"The default font is used after Last Download Manager restarts.");
        return true;
    case ID_HELP_CONTENTS:
        if (!context_.shell->OpenUrl(context_.owner, kHelpUrl)) {
            ShowError(L"The help page could not be opened.");
        }
        return true;
    case ID_HELP_ABOUT: {
        AboutDialog about(*context_.theme);
        about.RunModal(context_.owner);
        return true;
    }
    case ID_ROW_OPEN:
        OpenSelected();
        return true;
    case ID_ROW_OPEN_WITH:
        if (const auto* view = SingleSelection(); view && view->download.status == model::DownloadStatus::Completed) {
            context_.shell->OpenWith(context_.owner, FinalPath(view->download));
        }
        return true;
    case ID_ROW_OPEN_FOLDER:
        if (const auto* view = SingleSelection()) {
            if (!context_.shell->ShowInFolder(FinalPath(view->download))) {
                ShowError(L"The folder could not be opened.");
            }
        }
        return true;
    case ID_ROW_MOVE_RENAME:
        MoveSelected(context_.owner);
        return true;
    case ID_ROW_PROPERTIES:
        ShowProperties();
        return true;
    case ID_ROW_DOUBLE_CLICK_OPEN:
        ModifySettings([](model::Settings& s) { s.doubleClickAction = model::DoubleClickAction::Open; });
        return true;
    case ID_ROW_DOUBLE_CLICK_PROPERTIES:
        ModifySettings([](model::Settings& s) { s.doubleClickAction = model::DoubleClickAction::Properties; });
        return true;
    case ID_ROW_DEFAULT_ACTION:
        DefaultAction();
        return true;
    case ID_EDIT_SELECT_ALL:
        context_.list->SelectAll();
        return true;
    case ID_CATEGORY_ADD:
        EditCategory(std::nullopt);
        return true;
    case ID_CATEGORY_EDIT:
        if (categoryMenuTarget_) {
            EditCategory(categoryMenuTarget_);
        }
        return true;
    case ID_CATEGORY_DELETE:
        if (categoryMenuTarget_) {
            DeleteCategory(*categoryMenuTarget_);
        }
        return true;
    default:
        break;
    }
    if (id >= ID_VIEW_ARRANGE_FIRST && id <= ID_VIEW_ARRANGE_LAST) {
        SetSort(static_cast<SortKey>(id - ID_VIEW_ARRANGE_FIRST));
        return true;
    }
    if (id >= ID_TRAY_WINDOW_FIRST && id <= ID_TRAY_WINDOW_LAST) {
        const auto index = static_cast<size_t>(id - ID_TRAY_WINDOW_FIRST);
        if (index < trayWindows_.size() && ::IsWindow(trayWindows_[index])) {
            ::ShowWindow(trayWindows_[index], SW_RESTORE);
            ::SetForegroundWindow(trayWindows_[index]);
        }
        return true;
    }
    return false;
}

void MainController::OnLaunchRequest(const core::LaunchRequest& request)
{
    // --start-queue starts the main queue (checklist A-10); a launch with neither an address nor
    // it just shows LDM.
    if (request.startQueue) {
        queues_.StartMainQueue();
    }
    if (!request.url) {
        if (!request.tray && !request.startQueue) {
            context_.restore();
        }
        return;
    }
    adds_.AddAddress(ExternalAdd{*request.url, request.saveDir, request.fileName, request.silent, request.queueOnly});
}

void MainController::OnBrowserCapture(browser::Capture capture)
{
    if (captures_.OfferAsNewAddress(capture)) {
        return;  // continues the download that waited for a new address
    }
    ExternalAdd add;
    add.url = std::move(capture.url);
    add.fileName = std::move(capture.fileName);
    std::optional<core::PostData> post;
    if (capture.post) {
        post = core::PostData{std::move(capture.post->contentType), std::move(capture.post->body)};
    }
    add.browser = BrowserRequest{std::move(capture.referrer), std::move(capture.userAgent), std::move(capture.cookies),
                                 std::move(post), std::move(capture.mimeType), capture.chosen};
    adds_.AddAddress(add);
}

void MainController::OnStreamCapture(StreamCapture capture)
{
    ExternalAdd add;
    add.url = std::move(capture.manifestUrl);
    add.fileName = std::move(capture.fileName);
    // Picked by the user: cancelling it says nothing about the capture rules.
    add.browser = BrowserRequest{std::move(capture.pageUrl), std::move(capture.userAgent), std::move(capture.cookies),
                                 std::nullopt, std::nullopt, true};
    add.media = std::move(capture.media);
    adds_.AddAddress(add);
}

void MainController::WarnElevated()
{
    (void)ShowMessage(context_.owner, *context_.theme,
                      MessageRequest{L"LDM is running as administrator, so your browsers cannot hand downloads to it.\n\n"
                                     L"Browsers run with normal rights, and Windows does not let them reach a program "
                                     L"that runs as administrator. Exit LDM and start it again normally. If its "
                                     L"shortcut or its Compatibility settings say \"Run as administrator\", turn that "
                                     L"off.",
                                     MessageKind::Warning});
}

void MainController::OnBrowserLinks(BrowserSessions::LinksRequest request)
{
    adds_.AddPageLinks(std::move(request.links), request.connection, request.requestId);
}

void MainController::OnClipboardChanged()
{
    // A disabled owner means a modal dialog is open: never stack an offer on top of it.
    if (!::IsWindowEnabled(context_.owner)) {
        return;
    }
    const auto snapshot = Snapshot();
    if (!snapshot) {
        return;
    }
    if (auto url = clipboard_.AddressToOffer(context_.owner, snapshot->settings)) {
        adds_.AddUrl(std::move(*url));
    }
}

void MainController::OnDroppedUrls(std::vector<std::string> urls)
{
    if (urls.empty()) {
        return;
    }
    if (urls.size() == 1) {
        adds_.AddAddress(ExternalAdd{std::move(urls.front())});
        return;
    }
    adds_.AddList(std::move(urls), L"Add dropped addresses");
}

void MainController::SetSort(SortKey key)
{
    const SortSpec current = context_.list->Sort();
    const SortSpec sort{key, current.key == key ? current.descending : false};
    context_.list->SetSort(sort);
    const std::string stored = SerializeSort(sort);
    ModifySettings([stored](model::Settings& s) { s.listSortKey = stored; });
}

void MainController::OnMenuPopup(HMENU menu)
{
    const auto snapshot = Snapshot();
    if (!snapshot || queues_.FillPopup(menu)) {
        return;
    }
    const auto selection = Selection();
    const CommandState state = ComputeCommandState(selection, *snapshot);
    const model::Settings& settings = snapshot->settings;
    const auto enable = [menu](UINT id, bool enabled) { ::EnableMenuItem(menu, id, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED)); };
    const auto check = [menu](UINT id, bool checked) { ::CheckMenuItem(menu, id, MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED)); };
    enable(ID_FILE_STOP, state.stop);
    enable(ID_FILE_REMOVE, state.remove);
    enable(ID_FILE_DOWNLOAD_NOW, state.resume);
    enable(ID_FILE_REDOWNLOAD, state.redownload);
    enable(ID_DOWNLOADS_STOP_ALL, state.stopAll);
    enable(ID_DOWNLOADS_PAUSE_ALL, state.stopAll);
    enable(ID_DOWNLOADS_DELETE_COMPLETED, state.deleteCompleted);
    enable(ID_DOWNLOADS_LIMITER_ON, !snapshot->speedLimiterOn);
    enable(ID_DOWNLOADS_LIMITER_OFF, snapshot->speedLimiterOn);
    check(ID_VIEW_HIDE_CATEGORIES, settings.hideCategories);
    check(ID_VIEW_DARK_MODE, settings.darkModeSupport);
    check(ID_TASKS_DROP_TARGET, settings.showDropTarget);
    ::CheckMenuRadioItem(menu, ID_VIEW_TOOLBAR_LARGE, ID_VIEW_TOOLBAR_SMALL,
                         settings.largeToolbarButtons ? ID_VIEW_TOOLBAR_LARGE : ID_VIEW_TOOLBAR_SMALL, MF_BYCOMMAND);
    const UINT tray = settings.trayIconStyle == model::TrayIconStyle::Color     ? ID_VIEW_TRAY_COLOR
                      : settings.trayIconStyle == model::TrayIconStyle::Classic ? ID_VIEW_TRAY_CLASSIC
                                                                                : ID_VIEW_TRAY_HIDDEN;
    ::CheckMenuRadioItem(menu, ID_VIEW_TRAY_COLOR, ID_VIEW_TRAY_HIDDEN, tray, MF_BYCOMMAND);
    const UINT arrange = ID_VIEW_ARRANGE_FIRST + static_cast<UINT>(context_.list->Sort().key);
    ::CheckMenuRadioItem(menu, ID_VIEW_ARRANGE_FIRST, ID_VIEW_ARRANGE_LAST, arrange, MF_BYCOMMAND);
}

void MainController::OnSelectionChanged()
{
    const auto snapshot = Snapshot();
    if (!snapshot) {
        return;
    }
    const auto selection = Selection();
    const CommandState state = ComputeCommandState(selection, *snapshot);
    Toolbar& toolbar = *context_.toolbar;
    toolbar.SetEnabled(ID_FILE_DOWNLOAD_NOW, state.resume);
    toolbar.SetEnabled(ID_FILE_STOP, state.stop);
    toolbar.SetEnabled(ID_DOWNLOADS_STOP_ALL, state.stopAll);
    toolbar.SetEnabled(ID_FILE_REMOVE, state.remove);
    toolbar.SetEnabled(ID_DOWNLOADS_DELETE_COMPLETED, state.deleteCompleted);
}

void MainController::OnGrabberSnapshot(std::shared_ptr<const grabber::GrabberSnapshot> snapshot)
{
    if (scheduler_) {
        scheduler_->UpdateProjects(std::move(snapshot));
    }
}

void MainController::OnSnapshot(const core::Snapshot& snapshot)
{
    if (scheduler_) {
        scheduler_->Update(Snapshot());
    }
    transfers_.OnSnapshot(snapshot);
    adds_.OnSnapshot(snapshot);
    playlists_.OfferWaiting();
}

void MainController::OnNotifications(std::vector<core::Notification> notifications)
{
    for (const core::Notification& notification : notifications) {
        if (const auto* added = std::get_if<core::AddResult>(&notification)) {
            adds_.OnAddResult(*added);
        } else if (const auto* completed = std::get_if<core::DownloadCompleted>(&notification)) {
            transfers_.OnCompleted(*completed);
            playlists_.OnCompleted(*completed);
        } else if (const auto* failed = std::get_if<core::DownloadFailed>(&notification)) {
            transfers_.OnFailed(*failed);
            captures_.OnFailed(*failed);
            if (failed->failure.reason == core::FailureReason::AuthRequired) {
                AskForLogin(*failed);
            }
        } else if (const auto* error = std::get_if<core::CoreError>(&notification)) {
            ShowError(base::ToUtf16(error->message));
        } else if (const auto* quota = std::get_if<core::QuotaReached>(&notification)) {
            AskAboutQuota(*quota);
        } else if (const auto* started = std::get_if<core::QueueStarted>(&notification)) {
            transfers_.OnQueueStarted(*started);
        } else if (const auto* finished = std::get_if<core::QueueFinished>(&notification)) {
            transfers_.OnQueueFinished(*finished);
        } else if (const auto* stopped = std::get_if<core::QueueStopped>(&notification)) {
            transfers_.OnQueueStopped(*stopped);
        }
    }
}

void MainController::AskAboutQuota(const core::QuotaReached& reached)
{
    const std::wstring message =
        L"The download quota of " + std::to_wstring(reached.megabytes) + L" MB every " + std::to_wstring(reached.hours) +
        (reached.hours == 1 ? L" hour" : L" hours") + L" is used up.\n\nStop downloading until " +
        FormatDateTime(reached.periodEndsAtUnixMs) +
        L"? The downloads continue by themselves then.\n\nChoose No to keep downloading until then.";
    const int answer = ShowMessage(context_.owner, *context_.theme,
                                   MessageRequest{message, MessageKind::Information, MessageButtons::YesNo, true});
    Post(core::QuotaDecisionCommand{answer == IDNO});
}

void MainController::RemoveSelected()
{
    const auto selection = Selection();
    const auto snapshot = Snapshot();
    if (selection.empty() || !snapshot) {
        return;
    }
    const bool anyCompleted = std::any_of(selection.begin(), selection.end(), [](const auto* v) {
        return v->download.status == model::DownloadStatus::Completed;
    });
    const bool anyUnfinished = std::any_of(selection.begin(), selection.end(), [](const auto* v) {
        return v->download.status != model::DownloadStatus::Completed;
    });
    const model::Settings& settings = snapshot->settings;
    bool deleteFiles = false;
    if ((anyCompleted && settings.confirmDeleteCompleted) || (anyUnfinished && settings.confirmDeleteIncomplete)) {
        DeleteDialog dialog(*context_.theme, selection.size(), anyCompleted, anyUnfinished);
        const auto answer = dialog.Run(context_.owner);
        if (!answer) {
            return;
        }
        deleteFiles = answer->deleteFiles;
        if (answer->dontAskAgain) {
            ModifySettings([anyCompleted, anyUnfinished](model::Settings& s) {
                if (anyCompleted) {
                    s.confirmDeleteCompleted = false;
                }
                if (anyUnfinished) {
                    s.confirmDeleteIncomplete = false;
                }
            });
        }
    }
    Post(core::DeleteCommand{context_.list->SelectedIds(), deleteFiles});
}

void MainController::OpenSelected()
{
    const auto* view = SingleSelection();
    if (view == nullptr) {
        return;
    }
    if (view->download.status != model::DownloadStatus::Completed) {
        ShowError(L"This download is not complete yet. Open it when it has finished.");
        return;
    }
    if (!context_.shell->Open(context_.owner, FinalPath(view->download))) {
        ShowError(L"The file could not be opened. It may have been moved or deleted.");
    }
}

void MainController::DefaultAction()
{
    const auto snapshot = Snapshot();
    const auto* view = SingleSelection();
    if (!snapshot || view == nullptr) {
        return;
    }
    if (snapshot->settings.doubleClickAction == model::DoubleClickAction::Properties) {
        ShowProperties();
        return;
    }
    if (view->download.status == model::DownloadStatus::Downloading) {
        transfers_.OpenProgress(view->download.id);
        return;
    }
    OpenSelected();
}

void MainController::ShowProperties()
{
    const auto* view = SingleSelection();
    if (view == nullptr) {
        return;
    }
    const core::DownloadView copy = *view;  // the selection may change while the dialog is open
    PropertiesDialog dialog(*context_.theme, copy,
                            PropertiesDialog::Callbacks{
                                [this, path = FinalPath(copy.download)](HWND owner) {
                                    if (!context_.shell->Open(owner, path)) {
                                        ShowError(L"The file could not be opened.");
                                    }
                                },
                                [this](HWND owner) { MoveSelected(owner); }});
    if (auto update = dialog.Run(context_.owner)) {
        Post(std::move(*update));
    }
}

void MainController::MoveSelected(HWND owner)
{
    const auto* selected = SingleSelection();
    const auto snapshot = Snapshot();
    if (selected == nullptr || !snapshot) {
        return;
    }
    const model::Download download = selected->download;
    if (download.status == model::DownloadStatus::Downloading || download.status == model::DownloadStatus::Stopping ||
        download.status == model::DownloadStatus::Finalizing) {
        ShowError(L"Stop the download before moving or renaming it.");
        return;
    }
    MoveDialog dialog(*context_.theme, base::ToUtf16(download.saveDir), base::ToUtf16(download.fileName),
                      HistoryEntries(snapshot->settings.recentFolders));
    const auto choice = dialog.Run(owner);
    if (!choice) {
        return;
    }
    const std::filesystem::path target = std::filesystem::path(choice->folder) / choice->fileName;
    if (target == FinalPath(download)) {
        return;
    }
    // A completed file moves on disk first; an unfinished one only changes where it will be saved.
    if (download.status == model::DownloadStatus::Completed && !context_.shell->MoveItem(owner, FinalPath(download), target)) {
        ShowError(L"The file could not be moved. A file with that name may already exist there.");
        return;
    }
    Post(core::MoveDownloadCommand{download.id, base::ToUtf8(choice->folder), base::ToUtf8(choice->fileName)});
}

void MainController::Find(bool showDialog)
{
    if (!showDialog && !findOptions_.text.empty()) {
        if (!context_.list->Find(findOptions_, false)) {
            ::MessageBeep(MB_ICONASTERISK);
        }
        return;
    }
    if (find_ && find_->Handle() != nullptr) {
        ::SetForegroundWindow(find_->Handle());
        return;
    }
    find_ = std::make_unique<FindDialog>(
        *context_.theme, findOptions_,
        [this](const FindOptions& options) {
            findOptions_ = options;
            return context_.list->Find(options, false);
        },
        [this] {
            findOptions_ = find_->Options();
            find_.reset();  // called from the dialog's WM_DESTROY, after which it touches nothing
        });
    find_->Open(context_.owner);
}

void MainController::CustomizeColumns()
{
    ColumnsDialog dialog(*context_.theme, context_.list->Columns());
    if (const auto columns = dialog.Run(context_.owner)) {
        context_.list->SetColumns(*columns);
        if (context_.columnsChanged) {
            context_.columnsChanged(*columns);
        }
    }
}

void MainController::EditCategory(std::optional<model::CategoryId> id)
{
    const auto snapshot = Snapshot();
    if (!snapshot) {
        return;
    }
    model::Category category;
    std::string previousExtensions;
    if (id) {
        const auto it = std::find_if(snapshot->categories.begin(), snapshot->categories.end(),
                                     [&](const model::Category& c) { return c.id == *id; });
        if (it == snapshot->categories.end()) {
            return;
        }
        category = *it;
        previousExtensions = it->extensions;
    } else if (!snapshot->categories.empty()) {
        category.folder = snapshot->categories.front().folder;
    }
    CategoryDialog dialog(*context_.theme, category);
    auto edited = dialog.Run(context_.owner);
    if (!edited) {
        return;
    }
    const bool typesChanged = id && edited->extensions != previousExtensions && !edited->extensions.empty();
    Post(core::SaveCategoryCommand{*edited});
    // Offer to move existing downloads of the new types into the category (checklist W-03).
    if (typesChanged && Ask(L"Put the downloads in the list that have these file types into this category too?")) {
        Post(core::ReassignCategoryCommand{*id});
    }
}

void MainController::DeleteCategory(model::CategoryId id)
{
    if (Ask(L"Delete this category? Its downloads stay in the list under All Downloads.")) {
        Post(core::DeleteCategoryCommand{id});
    }
}

void MainController::SpeedLimiterSettings()
{
    const auto snapshot = Snapshot();
    if (!snapshot) {
        return;
    }
    SpeedLimitDialog dialog(*context_.theme, SpeedLimitDialog::Choice{snapshot->settings.speedLimiterKilobytesPerSecond,
                                                                      snapshot->settings.speedLimiterOnStartup});
    if (const auto choice = dialog.Run(context_.owner)) {
        Post(core::SetSpeedLimiterCommand{snapshot->speedLimiterOn, choice->kilobytesPerSecond});
        const bool onStartup = choice->onStartup;
        ModifySettings([onStartup](model::Settings& s) { s.speedLimiterOnStartup = onStartup; });
    }
}

void MainController::ShowOptions()
{
    const auto snapshot = Snapshot();
    if (!snapshot) {
        return;
    }
    OptionsDialog::Input input;
    input.settings = snapshot->settings;
    input.categories = snapshot->categories;
    input.serverLimits = snapshot->serverLimits;
    input.siteLogins = snapshot->siteLogins;
    input.launchOnStartup = context_.startup->IsEnabled();
    if (context_.connectedBrowsers) {
        input.connectedBrowsers = context_.connectedBrowsers();
    }
    OptionsDialog dialog(*context_.theme, std::move(input), *context_.secrets, *context_.system);
    auto output = dialog.Run(context_.owner);
    if (!output) {
        return;
    }
    ModifySettings([edited = output->settings](model::Settings& s) { OptionsDialog::ApplyEditedFields(edited, s); });
    for (model::Category& category : output->changedCategories) {
        Post(core::SaveCategoryCommand{std::move(category)});
    }
    if (output->serverLimits) {
        Post(core::ReplaceServerLimitsCommand{std::move(*output->serverLimits)});
    }
    if (output->siteLogins) {
        Post(core::ReplaceSiteLoginsCommand{std::move(*output->siteLogins)});
    }
    if (output->launchOnStartup != context_.startup->IsEnabled() && !context_.startup->SetEnabled(output->launchOnStartup)) {
        ShowError(L"Windows did not allow changing the startup setting.");
    }
}

void MainController::AskForLogin(const core::DownloadFailed& failed)
{
    const auto snapshot = Snapshot();
    if (!snapshot) {
        return;
    }
    const auto view = std::find_if(snapshot->downloads.begin(), snapshot->downloads.end(),
                                   [&](const auto& v) { return v->download.id == failed.id; });
    if (view == snapshot->downloads.end()) {
        return;
    }
    const auto url = core::ParseUrl((*view)->download.url);
    const std::wstring server = url ? base::ToUtf16(url->host) : std::wstring();
    LoginDialog dialog(*context_.theme, server, base::ToUtf16(failed.fileName));
    const auto answer = dialog.Run(context_.owner);
    if (!answer) {
        return;
    }
    core::UpdateDownloadCommand update;
    update.id = failed.id;
    update.loginUser = answer->user;
    update.loginPassword = answer->password;
    Post(update);
    if (answer->save && url) {
        // Existing logins keep their stored passwords (an empty password means unchanged).
        std::vector<core::SiteLoginInput> logins;
        for (const core::SiteLoginView& login : snapshot->siteLogins) {
            if (login.pattern != url->host) {
                logins.push_back(core::SiteLoginInput{login.pattern, login.username, {}});
            }
        }
        logins.push_back(core::SiteLoginInput{url->host, answer->user, answer->password});
        Post(core::ReplaceSiteLoginsCommand{std::move(logins)});
    }
    Post(core::ResumeCommand{{failed.id}});
}

void MainController::ChooseListFont()
{
    LOGFONTW font{};
    if (const auto current = reinterpret_cast<HFONT>(::SendMessageW(context_.list->Handle(), WM_GETFONT, 0, 0))) {
        ::GetObjectW(current, sizeof(font), &font);
    }
    CHOOSEFONTW choose{sizeof(choose)};
    choose.hwndOwner = context_.owner;
    choose.lpLogFont = &font;
    choose.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS;
    if (!::ChooseFontW(&choose)) {
        return;
    }
    const std::string setting = base::ToUtf8(font.lfFaceName) + ";" + std::to_string(choose.iPointSize / 10);
    ModifySettings([setting](model::Settings& s) { s.listFont = setting; });
    Tell(L"The new font is used after Last Download Manager restarts.");
}

void MainController::ShowRowMenu(POINT screen)
{
    const auto snapshot = Snapshot();
    if (!snapshot) {
        return;
    }
    const auto selection = Selection();
    if (selection.empty()) {
        return;
    }
    const CommandState state = ComputeCommandState(selection, *snapshot);
    UniqueMenu menu(::CreatePopupMenu());
    UniqueMenu doubleClick(::CreatePopupMenu());
    const bool openOnDoubleClick = snapshot->settings.doubleClickAction == model::DoubleClickAction::Open;
    AppendItem(doubleClick.get(), ID_ROW_DOUBLE_CLICK_OPEN, L"Open", true, openOnDoubleClick);
    AppendItem(doubleClick.get(), ID_ROW_DOUBLE_CLICK_PROPERTIES, L"Properties", true, !openOnDoubleClick);

    // Order per gap G-1.
    AppendItem(menu.get(), ID_ROW_OPEN, L"&Open", state.open);
    AppendItem(menu.get(), ID_ROW_OPEN_WITH, L"Open &with...", state.open);
    AppendItem(menu.get(), ID_ROW_OPEN_FOLDER, L"Open &folder", state.openFolder);
    AppendSeparator(menu.get());
    AppendItem(menu.get(), ID_ROW_MOVE_RENAME, L"&Move/Rename...\tCtrl+M", state.moveRename);
    AppendSeparator(menu.get());
    AppendItem(menu.get(), ID_FILE_REDOWNLOAD, L"R&edownload", state.redownload);
    AppendSeparator(menu.get());
    AppendItem(menu.get(), ID_FILE_DOWNLOAD_NOW, L"&Resume download", state.resume);
    AppendItem(menu.get(), ID_FILE_STOP, L"&Stop download", state.stop);
    AppendSeparator(menu.get());
    AppendItem(menu.get(), ID_ROW_REFRESH_ADDRESS, L"Refresh download &address", state.refreshAddress);
    AppendSeparator(menu.get());
    AppendItem(menu.get(), ID_FILE_REMOVE, L"Re&move\tDel", state.remove);
    AppendSeparator(menu.get());
    queues_.AppendRowItems(menu.get(), selection);
    AppendSeparator(menu.get());
    ::AppendMenuW(menu.get(), MF_POPUP, reinterpret_cast<UINT_PTR>(doubleClick.release()), L"On &double click");  // owned by `menu` now
    AppendSeparator(menu.get());
    AppendItem(menu.get(), ID_ROW_PROPERTIES, L"&Properties", state.properties);
    ::TrackPopupMenuEx(menu.get(), TPM_RIGHTBUTTON, screen.x, screen.y, context_.owner, nullptr);
}

void MainController::ShowCategoryMenu(std::optional<model::CategoryId> category, POINT screen)
{
    const auto snapshot = Snapshot();
    bool deletable = false;
    if (snapshot && category) {
        for (const model::Category& c : snapshot->categories) {
            deletable = deletable || (c.id == *category && !c.builtin);
        }
    }
    categoryMenuTarget_ = category;
    UniqueMenu menu(::CreatePopupMenu());
    AppendItem(menu.get(), ID_CATEGORY_ADD, L"&Add category...");
    AppendItem(menu.get(), ID_CATEGORY_EDIT, L"&Properties...", category.has_value());
    AppendItem(menu.get(), ID_CATEGORY_DELETE, L"&Delete category", deletable);
    ::TrackPopupMenuEx(menu.get(), TPM_RIGHTBUTTON, screen.x, screen.y, context_.owner, nullptr);
}

void MainController::OpenScheduler(std::optional<model::QueueId> select)
{
    if (scheduler_ && scheduler_->Handle() != nullptr) {
        scheduler_->Activate(select);
        return;
    }
    SchedulerWindow::Callbacks callbacks{[this](core::Command command) { Post(std::move(command)); },
                                         [this] { scheduler_.reset(); },  // from the window's WM_DESTROY, after which it touches nothing
                                         [this] { return context_.system->WakeTimersAllowed(); },
                                         {}};
    if (context_.grabber != nullptr) {
        callbacks.postGrabber = [this](core::GrabberCommand command) { context_.grabber->Post(std::move(command)); };
    }
    scheduler_ = std::make_unique<SchedulerWindow>(*context_.theme, std::move(callbacks));
    if (context_.grabberSnapshot) {
        scheduler_->UpdateProjects(context_.grabberSnapshot());
    }
    scheduler_->Open(context_.owner, Snapshot(), select);
    if (scheduler_ && scheduler_->Handle() == nullptr) {
        scheduler_.reset();
    }
}

void MainController::ShowQueueMenu(std::optional<model::QueueId> queue, POINT screen)
{
    queues_.ShowTreeMenu(queue, screen);
}

void MainController::ShowQueueToolbarMenu(int command, POINT screen)
{
    queues_.ShowToolbarMenu(command, screen);
}

void MainController::ShowTrayMenu(POINT screen)
{
    // The tray menu, in its order (gap G-2).
    const auto snapshot = Snapshot();
    const bool limiterOn = snapshot && snapshot->speedLimiterOn;
    const bool dropTargetShown = snapshot && snapshot->settings.showDropTarget;
    UniqueMenu menu(::CreatePopupMenu());
    UniqueMenu limiter(::CreatePopupMenu());
    AppendItem(limiter.get(), ID_DOWNLOADS_LIMITER_ON, L"Turn &on");
    AppendItem(limiter.get(), ID_DOWNLOADS_LIMITER_OFF, L"Turn o&ff");
    ::CheckMenuRadioItem(limiter.get(), ID_DOWNLOADS_LIMITER_ON, ID_DOWNLOADS_LIMITER_OFF,
                         limiterOn ? ID_DOWNLOADS_LIMITER_ON : ID_DOWNLOADS_LIMITER_OFF, MF_BYCOMMAND);
    AppendSeparator(limiter.get());
    AppendItem(limiter.get(), ID_DOWNLOADS_LIMITER_SETTINGS, L"&Settings...");

    AppendItem(menu.get(), ID_HELP_CONTENTS, L"&Help");
    AppendItem(menu.get(), ID_HELP_ABOUT, L"&About Last Download Manager");
    AppendSeparator(menu.get());
    ::AppendMenuW(menu.get(), MF_POPUP, reinterpret_cast<UINT_PTR>(limiter.release()), L"Speed &Limiter");  // owned by `menu` now
    AppendSeparator(menu.get());
    AppendItem(menu.get(), ID_TASKS_DROP_TARGET, L"Show &drop target", true, dropTargetShown);
    AppendItem(menu.get(), ID_TRAY_RESTORE, L"&Restore");  // not bold: double-clicking the icon restores anyway
    // Download windows, so a hidden or minimized progress window can be brought back (V-01).
    trayWindows_.clear();
    const auto windows = transfers_.Windows();
    if (!windows.empty()) {
        AppendSeparator(menu.get());
        for (const auto& [window, title] : windows) {
            if (ID_TRAY_WINDOW_FIRST + trayWindows_.size() > ID_TRAY_WINDOW_LAST) {
                break;
            }
            AppendItem(menu.get(), static_cast<UINT>(ID_TRAY_WINDOW_FIRST + trayWindows_.size()), title.c_str());
            trayWindows_.push_back(window);
        }
    }
    AppendSeparator(menu.get());
    AppendItem(menu.get(), ID_TASKS_EXIT, L"E&xit");
    // A tray menu must belong to the foreground window, or it will not close when clicking away.
    ::SetForegroundWindow(context_.owner);
    ::TrackPopupMenuEx(menu.get(), TPM_RIGHTBUTTON, screen.x, screen.y, context_.owner, nullptr);
    ::PostMessageW(context_.owner, WM_NULL, 0, 0);
}

}  // namespace ldm::ui
