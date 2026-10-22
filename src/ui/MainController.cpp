#include "ui/MainController.h"

#include "base/Utf.h"
#include "core/FileNames.h"
#include "core/Matching.h"
#include "core/Url.h"
#include "resource.h"
#include "ui/CommandState.h"
#include "ui/History.h"
#include "ui/dialogs/AddUrlDialog.h"
#include "ui/dialogs/ConfirmDialogs.h"
#include "ui/dialogs/DownloadInfoDialog.h"

#include <commdlg.h>

#include <algorithm>
#include <filesystem>

namespace ldm::ui {

namespace {

constexpr size_t kAddressHistoryLimit = 20;
constexpr size_t kFolderHistoryLimit = 10;
constexpr wchar_t kHelpUrl[] = L"https://github.com/xMags/Last-Download-Manager#readme";
constexpr wchar_t kAppTitle[] = L"Last Download Manager";

std::filesystem::path FinalPath(const model::Download& download)
{
    return std::filesystem::path(base::ToUtf16(download.saveDir)) / base::ToUtf16(download.fileName);
}

std::string ClipboardUrl(HWND owner)
{
    if (!::IsClipboardFormatAvailable(CF_UNICODETEXT) || !::OpenClipboard(owner)) {
        return {};
    }
    std::string text;
    if (const HANDLE data = ::GetClipboardData(CF_UNICODETEXT); data != nullptr) {
        if (const auto* chars = static_cast<const wchar_t*>(::GlobalLock(data)); chars != nullptr) {
            // Only a short, single address is worth prefilling; anything else is ignored.
            const size_t length = ::wcsnlen(chars, 4096);
            if (length < 4096) {
                text = base::ToUtf8(std::wstring_view(chars, length));
            }
            ::GlobalUnlock(data);
        }
    }
    ::CloseClipboard();
    const auto urls = core::ExtractUrls(text);
    return urls.size() == 1 ? urls.front() : std::string();
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

MainController::MainController(Context context) : context_(std::move(context)) {}

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
    ::MessageBoxW(context_.owner, message.c_str(), kAppTitle, MB_OK | MB_ICONWARNING);
}

bool MainController::OnCommand(int id)
{
    const auto selectedIds = [this] { return context_.list->SelectedIds(); };
    switch (id) {
    case ID_TASKS_ADD_URL:
        AddUrl(ClipboardUrl(context_.owner));
        return true;
    case ID_TASKS_EXIT:
        context_.exit();
        return true;
    case ID_TRAY_RESTORE:
        context_.restore();
        return true;
    case ID_FILE_DOWNLOAD_NOW:
        Post(core::ResumeCommand{selectedIds()});
        return true;
    case ID_FILE_STOP:
        Post(core::StopCommand{selectedIds()});
        return true;
    case ID_FILE_REMOVE:
        RemoveSelected();
        return true;
    case ID_FILE_REDOWNLOAD:
        if (const auto* view = SingleSelection()) {
            const bool complete = view->download.status == model::DownloadStatus::Completed;
            if (!complete || ::MessageBoxW(context_.owner,
                                           L"Download this file again? The existing file is replaced when the new copy "
                                           L"is complete.",
                                           kAppTitle, MB_YESNO | MB_ICONQUESTION) == IDYES) {
                Post(core::RedownloadCommand{view->download.id});
            }
        }
        return true;
    case ID_DOWNLOADS_PAUSE_ALL:
    case ID_DOWNLOADS_STOP_ALL:
        Post(core::StopAllCommand{});
        return true;
    case ID_DOWNLOADS_DELETE_COMPLETED:
        if (::MessageBoxW(context_.owner, L"Remove every completed download from the list? The files stay on disk.",
                          kAppTitle, MB_YESNO | MB_ICONQUESTION) == IDYES) {
            Post(core::DeleteCompletedCommand{});
        }
        return true;
    case ID_DOWNLOADS_LIMITER_ON:
        Post(core::SetSpeedLimiterCommand{true, std::nullopt});
        return true;
    case ID_DOWNLOADS_LIMITER_OFF:
        Post(core::SetSpeedLimiterCommand{false, std::nullopt});
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
    case ID_VIEW_DARK_MODE:
        ModifySettings([](model::Settings& s) { s.darkModeSupport = !s.darkModeSupport; });
        return true;
    case ID_VIEW_FONT_CHOOSE: {
        LOGFONTW font{};
        if (const auto current = reinterpret_cast<HFONT>(::SendMessageW(context_.list->Handle(), WM_GETFONT, 0, 0))) {
            ::GetObjectW(current, sizeof(font), &font);
        }
        CHOOSEFONTW choose{sizeof(choose)};
        choose.hwndOwner = context_.owner;
        choose.lpLogFont = &font;
        choose.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS;
        if (::ChooseFontW(&choose)) {
            const std::string setting = base::ToUtf8(font.lfFaceName) + ";" + std::to_string(choose.iPointSize / 10);
            ModifySettings([setting](model::Settings& s) { s.listFont = setting; });
            ::MessageBoxW(context_.owner, L"The new font is used after Last Download Manager restarts.", kAppTitle,
                          MB_OK | MB_ICONINFORMATION);
        }
        return true;
    }
    case ID_VIEW_FONT_RESET:
        ModifySettings([](model::Settings& s) { s.listFont.clear(); });
        return true;
    case ID_HELP_CONTENTS:
        if (!context_.shell->OpenUrl(context_.owner, kHelpUrl)) {
            ShowError(L"The help page could not be opened.");
        }
        return true;
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
    default:
        break;
    }
    if (id >= ID_VIEW_ARRANGE_FIRST && id <= ID_VIEW_ARRANGE_LAST) {
        SetSort(static_cast<SortKey>(id - ID_VIEW_ARRANGE_FIRST));
        return true;
    }
    return false;
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
    if (!snapshot) {
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

void MainController::OnSnapshot(const core::Snapshot& snapshot)
{
    for (auto& [token, pending] : pendingAdds_) {
        if (pending.dialog == nullptr || !pending.id) {
            continue;
        }
        for (const auto& view : snapshot.downloads) {
            if (view->download.id == *pending.id) {
                pending.dialog->Update(*view);
                break;
            }
        }
    }
}

void MainController::OnNotifications(std::vector<core::Notification> notifications)
{
    for (core::Notification& notification : notifications) {
        if (auto* added = std::get_if<core::AddResult>(&notification)) {
            const auto pending = pendingAdds_.find(added->token);
            if (added->error) {
                if (pending != pendingAdds_.end() && pending->second.dialog != nullptr) {
                    ::EndDialog(pending->second.dialog->Handle(), IDCANCEL);
                }
                ShowError(base::ToUtf16(*added->error));
            }
            if (added->showExistingComplete && added->id) {
                context_.list->SelectOnly(*added->id);
            }
            if (pending == pendingAdds_.end()) {
                continue;
            }
            pending->second.id = added->id;
            if (added->id) {
                if (pending->second.cancelled) {
                    Post(core::DeleteCommand{{*added->id}, true});
                } else if (pending->second.confirmation) {
                    pending->second.confirmation->id = *added->id;
                    Post(*pending->second.confirmation);
                }
            }
            if (pending->second.dialog == nullptr) {
                pendingAdds_.erase(pending);
            } else if (const auto snapshot = Snapshot()) {
                OnSnapshot(*snapshot);
            }
            continue;
        }
        if (const auto* error = std::get_if<core::CoreError>(&notification)) {
            ShowError(base::ToUtf16(error->message));
        }
    }
}

bool MainController::ResolveDuplicate(const std::string& url, std::optional<model::DuplicatePolicy>& policy)
{
    const auto snapshot = Snapshot();
    if (!snapshot || snapshot->settings.duplicatePolicy != model::DuplicatePolicy::Ask) {
        return true;
    }
    const auto existing = std::find_if(snapshot->downloads.begin(), snapshot->downloads.end(),
                                       [&](const auto& view) { return view->download.url == url; });
    if (existing == snapshot->downloads.end()) {
        return true;
    }
    DuplicateDialog duplicate(*context_.theme, base::ToUtf16((*existing)->download.fileName),
                              (*existing)->download.status == model::DownloadStatus::Completed);
    const auto answer = duplicate.Run(context_.owner);
    if (!answer) {
        return false;
    }
    policy = answer->policy;
    if (answer->remember) {
        const model::DuplicatePolicy chosen = answer->policy;
        ModifySettings([chosen](model::Settings& s) { s.duplicatePolicy = chosen; });
    }
    if (answer->policy == model::DuplicatePolicy::ResumeOrShowComplete) {
        core::AddDownloadCommand command;
        command.token = nextToken_++;
        command.url = url;
        command.duplicatePolicy = answer->policy;
        Post(std::move(command));
        return false;
    }
    return true;
}

void MainController::AddAddress(const std::string& url, bool silent)
{
    const auto snapshot = Snapshot();
    if (!snapshot) {
        return;
    }
    std::optional<model::DuplicatePolicy> policy;
    if (silent) {
        policy = model::DuplicatePolicy::AddNumbered;  // nobody to ask; never replace silently
    } else if (!ResolveDuplicate(url, policy)) {
        return;
    }
    if (silent || !snapshot->settings.showStartDialog) {
        core::AddDownloadCommand command;
        command.token = nextToken_++;
        command.url = url;
        command.duplicatePolicy = policy;
        command.mode = snapshot->settings.queueOnly ? core::AddMode::Later : core::AddMode::StartNow;
        Post(std::move(command));
        return;
    }
    AddWithInfoDialog(url, std::nullopt, std::nullopt, policy);
}

void MainController::AddUrl(std::string initialUrl)
{
    const auto snapshot = Snapshot();
    if (!snapshot) {
        return;
    }
    AddUrlDialog dialog(*context_.theme, HistoryEntries(snapshot->settings.addressHistory), std::move(initialUrl));
    const auto input = dialog.Run(context_.owner);
    if (!input) {
        return;
    }
    const std::string url = input->url;
    ModifySettings([url](model::Settings& s) { s.addressHistory = PushHistory(s.addressHistory, url, kAddressHistoryLimit); });

    // The duplicate question comes before the info dialog (checklist A-05).
    std::optional<model::DuplicatePolicy> policy;
    if (!ResolveDuplicate(url, policy)) {
        return;
    }
    if (!snapshot->settings.showStartDialog) {
        core::AddDownloadCommand command;
        command.token = nextToken_++;
        command.url = url;
        command.loginUser = input->user;
        command.loginPassword = input->password;
        command.mode = snapshot->settings.queueOnly ? core::AddMode::Later : core::AddMode::StartNow;
        command.duplicatePolicy = policy;
        Post(std::move(command));
        return;
    }
    AddWithInfoDialog(url, input->user, input->password, policy);
}

void MainController::AddWithInfoDialog(const std::string& url, std::optional<std::string> user,
                                       std::optional<std::string> password,
                                       std::optional<model::DuplicatePolicy> duplicatePolicy)
{
    const auto snapshot = Snapshot();
    const auto parsed = core::ParseUrl(url);
    if (!snapshot || !parsed || snapshot->categories.empty()) {
        return;
    }
    const model::Settings& settings = snapshot->settings;

    DownloadInfoDialog::Initial initial;
    initial.url = url;
    initial.fileName = core::SanitizeFileName(core::FileNameFromUrlPath(parsed->path));
    initial.categories = snapshot->categories;
    initial.category = core::MatchCategory(snapshot->categories, initial.fileName, *parsed);
    if (!initial.category) {
        const auto general = std::find_if(snapshot->categories.begin(), snapshot->categories.end(),
                                          [](const model::Category& c) { return c.builtin && c.extensions.empty(); });
        initial.category = general != snapshot->categories.end() ? general->id : snapshot->categories.front().id;
    }
    for (const model::Category& category : snapshot->categories) {
        if (category.id == *initial.category) {
            initial.saveDir = category.folder;
        }
    }
    initial.recentFolders = HistoryEntries(settings.recentFolders);

    const bool speculative = settings.speculativeStart;
    const std::uint64_t token = nextToken_++;
    if (speculative) {
        core::AddDownloadCommand command;
        command.token = token;
        command.url = url;
        command.loginUser = user;
        command.loginPassword = password;
        command.mode = core::AddMode::Speculative;
        command.duplicatePolicy = duplicatePolicy;
        pendingAdds_[token] = PendingAdd{token};
        Post(std::move(command));
    }

    DownloadInfoDialog dialog(*context_.theme, std::move(initial), nullptr);
    if (speculative) {
        pendingAdds_[token].dialog = &dialog;
    }
    const DownloadInfoDialog::Choice choice = dialog.Run(context_.owner);

    if (choice.action != DownloadInfoDialog::Action::Cancel) {
        const std::string folder = choice.saveDir;
        ModifySettings([folder](model::Settings& s) { s.recentFolders = PushHistory(s.recentFolders, folder, kFolderHistoryLimit); });
    }
    if (!speculative) {
        if (choice.action == DownloadInfoDialog::Action::Cancel) {
            return;
        }
        core::AddDownloadCommand command;
        command.token = token;
        command.url = url;
        command.fileName = choice.fileName;
        command.saveDir = choice.saveDir;
        command.categoryId = choice.category;
        command.description = choice.description;
        command.loginUser = user;
        command.loginPassword = password;
        command.mode = choice.action == DownloadInfoDialog::Action::Start ? core::AddMode::StartNow : core::AddMode::Later;
        command.duplicatePolicy = duplicatePolicy;
        Post(std::move(command));
        return;
    }

    const auto pending = pendingAdds_.find(token);
    if (pending == pendingAdds_.end()) {
        return;  // the add failed and its error was shown
    }
    pending->second.dialog = nullptr;
    if (choice.action == DownloadInfoDialog::Action::Cancel) {
        if (pending->second.id) {
            Post(core::DeleteCommand{{*pending->second.id}, true});
            pendingAdds_.erase(pending);
        } else {
            pending->second.cancelled = true;
        }
        return;
    }
    core::ConfirmDownloadCommand confirm;
    confirm.fileName = choice.fileName;
    confirm.saveDir = choice.saveDir;
    confirm.categoryId = choice.category;
    confirm.description = choice.description;
    confirm.start = choice.action == DownloadInfoDialog::Action::Start;
    confirm.rememberFolderForCategory = choice.rememberFolder;
    if (pending->second.id) {
        confirm.id = *pending->second.id;
        Post(confirm);
        pendingAdds_.erase(pending);
    } else {
        pending->second.confirmation = confirm;
    }
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
        OnCommand(ID_ROW_PROPERTIES);
        return;
    }
    OpenSelected();
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

    // Order captured from the reference (gap G-1); queue entries arrive with queues in M4.
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
    AppendItem(menu.get(), ID_ROW_REFRESH_ADDRESS, L"Refresh download &address", false);
    AppendSeparator(menu.get());
    AppendItem(menu.get(), ID_FILE_REMOVE, L"Re&move\tDel", state.remove);
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
    UniqueMenu menu(::CreatePopupMenu());
    AppendItem(menu.get(), ID_CATEGORY_ADD, L"&Add category...");
    AppendItem(menu.get(), ID_CATEGORY_EDIT, L"&Edit category...", category.has_value());
    AppendItem(menu.get(), ID_CATEGORY_DELETE, L"&Delete category", deletable);
    ::TrackPopupMenuEx(menu.get(), TPM_RIGHTBUTTON, screen.x, screen.y, context_.owner, nullptr);
}

void MainController::ShowTrayMenu(POINT screen)
{
    const auto snapshot = Snapshot();
    const bool limiterOn = snapshot && snapshot->speedLimiterOn;
    UniqueMenu menu(::CreatePopupMenu());
    UniqueMenu limiter(::CreatePopupMenu());
    AppendItem(limiter.get(), ID_DOWNLOADS_LIMITER_ON, L"Turn o&n", !limiterOn);
    AppendItem(limiter.get(), ID_DOWNLOADS_LIMITER_OFF, L"Turn o&ff", limiterOn);
    AppendItem(menu.get(), ID_TRAY_RESTORE, L"&Open Last Download Manager");
    ::SetMenuDefaultItem(menu.get(), ID_TRAY_RESTORE, FALSE);
    AppendSeparator(menu.get());
    AppendItem(menu.get(), ID_TASKS_ADD_URL, L"&Add new download...");
    ::AppendMenuW(menu.get(), MF_POPUP, reinterpret_cast<UINT_PTR>(limiter.release()), L"Speed &limiter");  // owned by `menu` now
    AppendItem(menu.get(), ID_DOWNLOADS_STOP_ALL, L"&Stop all downloads", snapshot && snapshot->activeDownloads > 0);
    AppendSeparator(menu.get());
    AppendItem(menu.get(), ID_TASKS_EXIT, L"E&xit");
    // A tray menu must belong to the foreground window, or it will not close when clicking away.
    ::SetForegroundWindow(context_.owner);
    ::TrackPopupMenuEx(menu.get(), TPM_RIGHTBUTTON, screen.x, screen.y, context_.owner, nullptr);
    ::PostMessageW(context_.owner, WM_NULL, 0, 0);
}

}  // namespace ldm::ui
