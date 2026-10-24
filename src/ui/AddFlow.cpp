#include "ui/AddFlow.h"

#include "base/Utf.h"
#include "core/FileNames.h"
#include "core/Matching.h"
#include "core/Url.h"
#include "ui/Clipboard.h"
#include "ui/History.h"
#include "ui/PageLinks.h"
#include "ui/dialogs/AddUrlDialog.h"
#include "ui/dialogs/AllLinksDialog.h"
#include "ui/dialogs/BatchDialog.h"
#include "ui/dialogs/CategoryDialog.h"
#include "ui/dialogs/ConfirmDialogs.h"
#include "ui/dialogs/DownloadInfoDialog.h"
#include "ui/dialogs/ListDialogs.h"
#include "ui/dialogs/MessageDialog.h"

#include <algorithm>
#include <filesystem>
#include <set>

namespace ldm::ui {

namespace {

constexpr size_t kAddressHistoryLimit = 20;
constexpr size_t kFolderHistoryLimit = 10;
// Clipboard text worth scanning for addresses; larger texts are ignored.
constexpr size_t kClipboardLimit = 1024 * 1024;

std::optional<model::CategoryId> GeneralCategory(const std::vector<model::Category>& categories)
{
    const auto general = std::find_if(categories.begin(), categories.end(),
                                      [](const model::Category& c) { return c.builtin && c.extensions.empty(); });
    if (general != categories.end()) {
        return general->id;
    }
    return categories.empty() ? std::nullopt : std::optional<model::CategoryId>(categories.front().id);
}

}  // namespace

AddFlow::AddFlow(Context context) : context_(std::move(context)) {}

AddFlow::~AddFlow() = default;

void AddFlow::Post(core::Command command)
{
    context_.commands->Post(std::move(command));
}

void AddFlow::ModifySettings(std::function<void(model::Settings&)> modify)
{
    Post(core::ModifySettingsCommand{std::move(modify)});
}

void AddFlow::ShowError(const std::wstring& message) const
{
    (void)ShowMessage(context_.owner, *context_.theme, MessageRequest{message, MessageKind::Warning});
}

bool AddFlow::ResolveDuplicate(const std::string& url, std::optional<model::DuplicatePolicy>& policy,
                               const core::MediaChoice* media)
{
    const auto snapshot = context_.snapshot();
    if (!snapshot || snapshot->settings.duplicatePolicy != model::DuplicatePolicy::Ask) {
        return true;
    }
    const auto sameTracks = [media](const model::Download& download) {
        if (media == nullptr || !download.IsStream()) {
            return media == nullptr && !download.IsStream();
        }
        return std::equal(download.tracks.begin(), download.tracks.end(), media->tracks.begin(), media->tracks.end(),
                          [](const model::MediaTrack& a, const core::MediaTrackChoice& b) { return a.sourceUrl == b.sourceUrl; });
    };
    const auto existing = std::find_if(snapshot->downloads.begin(), snapshot->downloads.end(), [&](const auto& view) {
        return view->download.url == url && sameTracks(view->download);
    });
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
        if (media != nullptr) {
            command.media = *media;  // finds the same stream, not a file at its manifest's address
        }
        Post(std::move(command));
        return false;
    }
    return true;
}

void AddFlow::AddAddress(const ExternalAdd& add)
{
    const auto snapshot = context_.snapshot();
    if (!snapshot || !core::ParseUrl(add.url)) {
        return;
    }
    std::optional<model::DuplicatePolicy> policy;
    if (add.silent) {
        policy = model::DuplicatePolicy::AddNumbered;  // nobody to ask; never replace silently
    } else if (!ResolveDuplicate(add.url, policy, add.media ? &*add.media : nullptr)) {
        return;
    }
    if (add.silent || !snapshot->settings.showStartDialog) {
        core::AddDownloadCommand command;
        command.token = nextToken_++;
        command.url = add.url;
        command.saveDir = add.saveDir;
        command.fileName = add.fileName;
        command.duplicatePolicy = policy;
        ApplyBrowserRequest(add.browser, command);
        command.media = add.media;
        command.mode = add.queueOnly || snapshot->settings.queueOnly ? core::AddMode::Later : core::AddMode::StartNow;
        if (!add.silent && command.mode == core::AddMode::StartNow) {
            progressOnAdd_.push_back(command.token);
        }
        Post(std::move(command));
        return;
    }
    AddWithInfoDialog(
        InfoDialogInput{add.url, std::nullopt, std::nullopt, policy, add.saveDir, add.fileName, add.browser, add.media});
}

void AddFlow::ApplyBrowserRequest(const std::optional<BrowserRequest>& browser, core::AddDownloadCommand& command)
{
    if (!browser) {
        return;
    }
    command.referer = browser->referrer;
    command.parentPage = browser->referrer;
    command.userAgent = browser->userAgent;
    command.cookies = browser->cookies;
    command.post = browser->post;
    command.contentType = browser->mimeType;
    captureTokens_.insert(command.token);
}

void AddFlow::AddImported(const std::vector<ImportedEntry>& entries)
{
    for (const ImportedEntry& entry : entries) {
        core::AddDownloadCommand command;
        command.token = nextToken_++;
        command.url = entry.url;
        command.fileName = entry.fileName;
        command.description = entry.description;
        command.referer = entry.referer;
        command.mode = core::AddMode::Later;
        command.duplicatePolicy = model::DuplicatePolicy::AddNumbered;
        Post(std::move(command));
    }
}

void AddFlow::AddUrl(std::string initialUrl)
{
    const auto snapshot = context_.snapshot();
    if (!snapshot) {
        return;
    }
    if (initialUrl.empty()) {
        // A single address on the clipboard is offered.
        const auto urls = core::ExtractUrls(base::ToUtf8(ReadClipboardText(context_.owner, 4096)));
        if (urls.size() == 1) {
            initialUrl = urls.front();
        }
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
        if (command.mode == core::AddMode::StartNow) {
            progressOnAdd_.push_back(command.token);
        }
        Post(std::move(command));
        return;
    }
    AddWithInfoDialog(InfoDialogInput{url, input->user, input->password, policy, std::nullopt, std::nullopt});
}

void AddFlow::AddWithInfoDialog(InfoDialogInput input)
{
    const std::string& url = input.url;
    std::optional<std::string>& user = input.user;
    std::optional<std::string>& password = input.password;
    const std::optional<model::DuplicatePolicy>& duplicatePolicy = input.duplicatePolicy;
    const auto snapshot = context_.snapshot();
    const auto parsed = core::ParseUrl(url);
    if (!snapshot || !parsed || snapshot->categories.empty()) {
        return;
    }
    const model::Settings& settings = snapshot->settings;

    DownloadInfoDialog::Initial initial;
    initial.url = url;
    initial.fileName = core::SanitizeFileName(core::FileNameFromUrlPath(parsed->path));
    if (input.fileName) {
        initial.fileName = *input.fileName;  // the category follows the chosen name
        initial.fileNameFixed = true;
    }
    initial.categories = snapshot->categories;
    initial.category = core::MatchCategory(snapshot->categories, initial.fileName, *parsed);
    if (!initial.category) {
        initial.category = GeneralCategory(snapshot->categories);
    }
    for (const model::Category& category : snapshot->categories) {
        if (category.id == *initial.category) {
            initial.saveDir = category.folder;
        }
    }
    if (input.saveDir) {
        initial.saveDir = *input.saveDir;
        initial.saveDirFixed = true;
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
        command.saveDir = input.saveDir;  // a folder or name the dialog does not let the user change
        command.fileName = input.fileName;
        ApplyBrowserRequest(input.browser, command);
        command.media = input.media;
        pendingAdds_[token] = PendingAdd{token};
        Post(std::move(command));
    }

    // "+" next to the category: the new category arrives with a later snapshot and is selected then.
    const auto addCategory = [this, snapshot](HWND owner) -> std::optional<model::Category> {
        model::Category category;
        const auto general = GeneralCategory(snapshot->categories);
        for (const model::Category& c : snapshot->categories) {
            if (general && c.id == *general) {
                category.folder = c.folder;
            }
        }
        CategoryDialog dialog(*context_.theme, std::move(category));
        if (auto created = dialog.Run(owner)) {
            categoryToSelect_ = created->name;
            Post(core::SaveCategoryCommand{std::move(*created)});
        }
        return std::nullopt;
    };
    DownloadInfoDialog dialog(*context_.theme, std::move(initial), addCategory);
    pendingAdds_[token].token = token;
    pendingAdds_[token].dialog = &dialog;
    const DownloadInfoDialog::Choice choice = dialog.Run(context_.owner);
    categoryToSelect_.reset();
    // Only downloads LDM took on its own count toward offering an exception (checklist B-06).
    if (choice.action == DownloadInfoDialog::Action::Cancel && input.browser && !input.browser->chosen &&
        context_.captureCancelled) {
        context_.captureCancelled(url);
    }

    if (choice.action != DownloadInfoDialog::Action::Cancel) {
        const std::string folder = choice.saveDir;
        ModifySettings([folder](model::Settings& s) { s.recentFolders = PushHistory(s.recentFolders, folder, kFolderHistoryLimit); });
    }
    const auto pending = pendingAdds_.find(token);
    if (!speculative) {
        if (pending != pendingAdds_.end()) {
            pendingAdds_.erase(pending);
        }
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
        ApplyBrowserRequest(input.browser, command);
        command.media = input.media;
        if (command.mode == core::AddMode::StartNow) {
            progressOnAdd_.push_back(token);
        }
        Post(std::move(command));
        return;
    }

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
        if (confirm.start) {
            context_.openProgress(confirm.id);
        }
        pendingAdds_.erase(pending);
        return;
    }
    if (confirm.start) {
        progressOnAdd_.push_back(token);
    }
    pending->second.confirmation = confirm;
}

void AddFlow::AddMany(const std::vector<std::string>& urls, bool start, std::optional<std::string> user,
                      std::optional<std::string> password)
{
    for (const std::string& url : urls) {
        core::AddDownloadCommand command;
        command.token = nextToken_++;
        command.url = url;
        command.loginUser = user;
        command.loginPassword = password;
        command.mode = start ? core::AddMode::StartNow : core::AddMode::Later;
        // A batch never stops to ask about each duplicate.
        command.duplicatePolicy = model::DuplicatePolicy::AddNumbered;
        Post(std::move(command));
    }
}

void AddFlow::AddBatch()
{
    BatchDialog dialog(*context_.theme);
    if (const auto choice = dialog.Run(context_.owner)) {
        AddMany(choice->urls, choice->start, choice->user, choice->password);
    }
}

void AddFlow::AddFromClipboard()
{
    const std::wstring text = ReadClipboardText(context_.owner, kClipboardLimit);
    std::vector<std::string> urls = core::ExtractUrls(base::ToUtf8(text));
    if (urls.empty()) {
        ShowError(L"The clipboard holds no download addresses.");
        return;
    }
    AddList(std::move(urls), L"Add batch download from clipboard");
}

void AddFlow::AddList(std::vector<std::string> urls, const std::wstring& title)
{
    UrlListDialog dialog(*context_.theme, title, std::move(urls));
    if (const auto choice = dialog.Run(context_.owner); choice && !choice->urls.empty()) {
        AddMany(choice->urls, choice->start, std::nullopt, std::nullopt);
    }
}

void AddFlow::ExpectLinks(BrowserConnectionId connection)
{
    earlyLinkInfo_.try_emplace(connection);
}

void AddFlow::OnLinkInfo(BrowserConnectionId connection, browser::LinkInfo info)
{
    if (linksDialog_ != nullptr && connection == linksConnection_) {
        linksDialog_->ApplyDetails(info.items);
        return;
    }
    const auto early = earlyLinkInfo_.find(connection);
    if (early == earlyLinkInfo_.end()) {
        return;  // no dialog waits for this connection
    }
    // Bounded: a link may be reported more than once, but not without end.
    std::vector<browser::LinkDetails>& items = early->second.items;
    constexpr size_t kLimit = browser::kMaxPageLinks * 2;
    const size_t room = kLimit - std::min(items.size(), kLimit);
    const size_t take = std::min(room, info.items.size());
    items.insert(items.end(), std::make_move_iterator(info.items.begin()),
                 std::make_move_iterator(info.items.begin() + static_cast<std::ptrdiff_t>(take)));
}

void AddFlow::OnBrowserClosed(BrowserConnectionId connection)
{
    if (linksDialog_ != nullptr && connection == linksConnection_) {
        linksDialog_->StopChecking();
    }
    if (const auto early = earlyLinkInfo_.find(connection); early != earlyLinkInfo_.end()) {
        early->second.closed = true;
    }
}

void AddFlow::AddPageLinks(browser::Links links, BrowserConnectionId connection, const std::string& requestId)
{
    EarlyLinkInfo early;
    if (const auto found = earlyLinkInfo_.find(connection); found != earlyLinkInfo_.end()) {
        early = std::move(found->second);
        earlyLinkInfo_.erase(found);
    }
    const auto snapshot = context_.snapshot();
    if (!snapshot || snapshot->categories.empty()) {
        if (context_.linksDone) {
            context_.linksDone(connection, requestId);
        }
        return;
    }
    const model::Settings& settings = snapshot->settings;

    std::set<std::string> listed;
    for (const auto& view : snapshot->downloads) {
        listed.insert(view->download.url);
    }
    AllLinksDialog::Initial initial;
    initial.rows = MakeLinkRows(links, listed);
    for (const browser::LinkDetails& details : early.items) {
        (void)ApplyLinkDetails(initial.rows, details);
    }
    initial.selection = links.selection;
    initial.filter = LinkFilter{settings.allLinksHideHtml, settings.allLinksHideImages, settings.allLinksHideRepeated};
    initial.useLinkText = settings.allLinksUseLinkText;
    initial.categories = snapshot->categories;
    initial.category = GeneralCategory(snapshot->categories);
    const std::vector<std::string> folders = HistoryEntries(settings.recentFolders);
    if (!folders.empty()) {
        initial.folder = folders.front();
    } else {
        for (const model::Category& category : snapshot->categories) {
            if (initial.category == category.id) {
                initial.folder = category.folder;
            }
        }
    }

    AllLinksDialog dialog(*context_.theme, std::move(initial));
    if (early.closed) {
        dialog.StopChecking();
    }
    linksConnection_ = connection;
    linksDialog_ = &dialog;
    const auto choice = dialog.Run(context_.owner);
    linksDialog_ = nullptr;
    linksConnection_ = 0;
    if (context_.linksDone) {
        context_.linksDone(connection, requestId);
    }

    const LinkFilter filter = dialog.Filter();
    const bool useLinkText = dialog.UseLinkText();
    if (filter.hideHtml != settings.allLinksHideHtml || filter.hideImages != settings.allLinksHideImages ||
        filter.hideRepeated != settings.allLinksHideRepeated || useLinkText != settings.allLinksUseLinkText) {
        ModifySettings([filter, useLinkText](model::Settings& s) {
            s.allLinksHideHtml = filter.hideHtml;
            s.allLinksHideImages = filter.hideImages;
            s.allLinksHideRepeated = filter.hideRepeated;
            s.allLinksUseLinkText = useLinkText;
        });
    }
    if (!choice) {
        return;
    }
    if (choice->saveTo == AllLinksDialog::SaveTo::OneFolder) {
        const std::string folder = choice->folder;
        ModifySettings([folder](model::Settings& s) { s.recentFolders = PushHistory(s.recentFolders, folder, kFolderHistoryLimit); });
    }
    for (const AllLinksDialog::Pick& pick : choice->picks) {
        core::AddDownloadCommand command;
        command.token = nextToken_++;
        command.url = pick.url;
        command.fileName = pick.fileName;
        command.description = pick.description;
        if (choice->saveTo == AllLinksDialog::SaveTo::OneCategory) {
            command.categoryId = choice->category;
        } else if (choice->saveTo == AllLinksDialog::SaveTo::OneFolder) {
            command.saveDir = choice->folder;
        }
        command.referer = links.referrer;
        command.parentPage = links.referrer;
        command.userAgent = links.userAgent;
        command.cookies = CookiesForAddress(links.cookies, pick.url);
        // Added to the list without starting.
        command.mode = core::AddMode::Later;
        command.duplicatePolicy = model::DuplicatePolicy::AddNumbered;
        Post(std::move(command));
    }
}

void AddFlow::OnSnapshot(const core::Snapshot& snapshot)
{
    for (auto& [token, pending] : pendingAdds_) {
        if (pending.dialog == nullptr) {
            continue;
        }
        if (categoryToSelect_) {
            const auto added = std::find_if(snapshot.categories.begin(), snapshot.categories.end(),
                                            [this](const model::Category& c) { return c.name == *categoryToSelect_; });
            if (added != snapshot.categories.end()) {
                pending.dialog->SetCategories(snapshot.categories, added->id);
                categoryToSelect_.reset();
            }
        }
        if (!pending.id) {
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

void AddFlow::OnAddResult(const core::AddResult& result)
{
    if (captureTokens_.erase(result.token) > 0 && result.id && !result.error && context_.capturedAdded) {
        context_.capturedAdded(*result.id);
    }
    const auto pending = pendingAdds_.find(result.token);
    if (result.error) {
        if (pending != pendingAdds_.end() && pending->second.dialog != nullptr) {
            ::EndDialog(pending->second.dialog->Handle(), IDCANCEL);
        }
        ShowError(base::ToUtf16(*result.error));
    }
    if (result.showExistingComplete && result.id) {
        context_.select(*result.id);
    }
    if (const auto wanted = std::find(progressOnAdd_.begin(), progressOnAdd_.end(), result.token);
        wanted != progressOnAdd_.end()) {
        progressOnAdd_.erase(wanted);
        if (result.id && !result.error) {
            context_.openProgress(*result.id);
        }
    }
    if (pending == pendingAdds_.end()) {
        return;
    }
    pending->second.id = result.id;
    if (result.id) {
        if (pending->second.cancelled) {
            Post(core::DeleteCommand{{*result.id}, true});
        } else if (pending->second.confirmation) {
            pending->second.confirmation->id = *result.id;
            Post(*pending->second.confirmation);
        }
    }
    if (pending->second.dialog == nullptr) {
        pendingAdds_.erase(pending);
    } else if (const auto snapshot = context_.snapshot()) {
        OnSnapshot(*snapshot);
    }
}

}  // namespace ldm::ui
