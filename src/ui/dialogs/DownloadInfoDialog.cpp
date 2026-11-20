#include "ui/dialogs/DownloadInfoDialog.h"

#include "base/Utf.h"
#include "core/FileNames.h"
#include "core/Url.h"
#include "resource.h"
#include "ui/Clipboard.h"
#include "ui/Dpi.h"
#include "ui/FileDialogs.h"
#include "ui/FileKinds.h"
#include "ui/FileTypeInfo.h"
#include "ui/Format.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <limits>

namespace ldm::ui {

namespace {

// Dialog units of the footer band, from the template's layout.
constexpr int kFooterDialogUnits = 37;
// Between the tags and the line after them, in dialog units.
constexpr int kTagGap = 3;
// Posted when the folder box changed, to show the free space of the new folder's drive.
constexpr UINT kFolderChangedMessage = WM_APP + 0x110;
// Icon-font glyphs.
constexpr wchar_t kAddGlyph = 0xE710;     // Add
constexpr wchar_t kFolderGlyph = 0xE8B7;  // Folder
constexpr wchar_t kLinkGlyph = 0xE71B;    // Link
// Heights of the drawn text boxes and pickers at 96 DPI, matching their template rectangles.
constexpr int kNameField96 = 36;
constexpr int kBoxField96 = 32;
// Folder menu commands after the recent folders.
constexpr UINT kBrowseCommand = 0x1000;
constexpr UINT kTypePathCommand = 0x1001;
// Recent folders listed in the folder menu.
constexpr size_t kMenuFolders = 10;

// "212 GB free" for the drive holding `folder`; empty for a network drive (asking could block the
// window) or when Windows cannot tell.
std::wstring FreeSpaceText(const std::wstring& folder)
{
    const std::filesystem::path path(folder);
    if (!path.is_absolute() || folder.starts_with(L"\\\\")) {
        return {};
    }
    const std::wstring root = path.root_path().wstring();
    const UINT type = ::GetDriveTypeW(root.c_str());
    if (type == DRIVE_REMOTE || type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN) {
        return {};
    }
    ULARGE_INTEGER available{};
    if (!::GetDiskFreeSpaceExW(root.c_str(), &available, nullptr, nullptr)) {
        return {};
    }
    const auto bytes = static_cast<std::int64_t>(
        std::min<unsigned long long>(available.QuadPart, static_cast<unsigned long long>(std::numeric_limits<std::int64_t>::max())));
    return FormatSize(bytes) + L" free";
}

// Menu text shows '&' as an access key unless doubled.
std::wstring MenuText(std::wstring text)
{
    for (size_t at = text.find(L'&'); at != std::wstring::npos; at = text.find(L'&', at + 2)) {
        text.insert(at, 1, L'&');
    }
    return text;
}

bool SamePath(const std::wstring& a, const std::wstring& b)
{
    return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

}  // namespace

DownloadInfoDialog::DownloadInfoDialog(const Theme& theme, Initial initial,
                                       std::function<std::optional<model::Category>(HWND)> addCategory)
    : Dialog(IDD_DOWNLOAD_INFO, theme), initial_(std::move(initial)), addCategory_(std::move(addCategory)), surface_(theme)
{
    UseSurface(&surface_);
}

DownloadInfoDialog::Choice DownloadInfoDialog::Run(HWND owner)
{
    RunModal(owner);
    return choice_;
}

bool DownloadInfoDialog::OnInit()
{
    downloads_ = DownloadsFolder();
    surface_.Attach(Handle());
    surface_.SetFooterHeight(kFooterDialogUnits);
    surface_.AddPanel(IDC_INFO_PANEL);
    surface_.AddButton(IDC_INFO_START, ButtonRole::Primary);
    surface_.AddButton(IDC_INFO_LATER, ButtonRole::Secondary);
    surface_.AddButton(IDCANCEL, ButtonRole::Secondary);
    surface_.AddButton(IDC_INFO_CATEGORY, ButtonRole::Picker);
    surface_.AddButton(IDC_INFO_FOLDER_PICKER, ButtonRole::Picker);
    surface_.SetButtonIcon(IDC_INFO_FOLDER_PICKER, kFolderGlyph);
    surface_.AddButton(IDC_INFO_ADD_CATEGORY, ButtonRole::Secondary);
    surface_.SetButtonGlyph(IDC_INFO_ADD_CATEGORY, kAddGlyph);
    surface_.AddButton(IDC_INFO_BROWSE, ButtonRole::Secondary);
    surface_.AddButton(IDC_INFO_COPY, ButtonRole::Link);
    surface_.AddButton(IDC_INFO_ADD_DESCRIPTION, ButtonRole::Link);
    surface_.AddChip(IDC_INFO_SIZE);
    surface_.AddChip(IDC_INFO_TYPE);
    surface_.AddIcon(IDC_INFO_LINK_ICON, kLinkGlyph);
    surface_.SetTextRole(IDC_INFO_NAME, TextRole::Heading);
    surface_.SetTextRole(IDC_INFO_HOST, TextRole::Caption);
    surface_.SetTextRole(IDC_INFO_URL, TextRole::Caption);
    surface_.SetTextRole(IDC_INFO_FREE, TextRole::Caption);
    surface_.SetTextRole(IDC_INFO_COPY, TextRole::Small);
    surface_.SetTextRole(IDC_INFO_ADD_DESCRIPTION, TextRole::Small);
    surface_.SetTextRole(IDC_INFO_CATEGORY_LABEL, TextRole::Muted);
    surface_.SetTextRole(IDC_INFO_FOLDER_LABEL, TextRole::Muted);
    // After the heading font, so the box fits the larger line of text.
    surface_.AddField(IDC_INFO_NAME, kNameField96);
    surface_.AddField(IDC_INFO_FOLDER, kBoxField96);
    surface_.AddField(IDC_INFO_DESCRIPTION, kBoxField96);
    Edit_SetCueBannerText(Item(IDC_INFO_DESCRIPTION), L"Description");

    settingText_ = true;
    nameEdited_ = initial_.fileNameFixed;
    folderEdited_ = initial_.saveDirFixed;
    SetText(IDC_INFO_URL, base::ToUtf16(initial_.url));
    SetText(IDC_INFO_NAME, base::ToUtf16(initial_.fileName));
    settingText_ = false;
    SetFolder(base::ToUtf16(initial_.saveDir));
    category_ = FindCategory(initial_.category) != nullptr
                    ? initial_.category
                    : (initial_.categories.empty() ? std::nullopt : std::optional(initial_.categories.front().id));
    ShowCategory();

    if (const auto url = core::ParseUrl(initial_.url); url && !url->host.empty()) {
        source_ = L"from " + base::ToUtf16(url->host);
    }
    ShowFileType(base::ToUtf16(initial_.fileName));
    ShowDetails(L"checking\u2026", source_, false);
    UpdateRememberText();
    ::SetFocus(Item(IDC_INFO_START));
    return false;
}

const model::Category* DownloadInfoDialog::FindCategory(std::optional<model::CategoryId> id) const
{
    if (!id) {
        return nullptr;
    }
    const auto found = std::find_if(initial_.categories.begin(), initial_.categories.end(),
                                    [&id](const model::Category& category) { return category.id == *id; });
    return found == initial_.categories.end() ? nullptr : &*found;
}

std::optional<model::CategoryId> DownloadInfoDialog::SelectedCategory() const
{
    return FindCategory(category_) != nullptr ? category_ : std::nullopt;
}

void DownloadInfoDialog::ShowCategory()
{
    const model::Category* category = FindCategory(category_);
    SetText(IDC_INFO_CATEGORY, category != nullptr ? base::ToUtf16(category->name) : std::wstring());
    surface_.SetButtonIcon(IDC_INFO_CATEGORY,
                           CategoryGlyph(category != nullptr ? KindOfCategory(*category) : FileKind::Other));
    ::InvalidateRect(Item(IDC_INFO_CATEGORY), nullptr, TRUE);
}

void DownloadInfoDialog::ChooseCategory()
{
    UniqueMenu menu(::CreatePopupMenu());
    for (size_t i = 0; i < initial_.categories.size(); ++i) {
        const model::Category& category = initial_.categories[i];
        const UINT flags = MF_STRING | (category_ == category.id ? MF_CHECKED : MF_UNCHECKED);
        ::AppendMenuW(menu.get(), flags, i + 1, MenuText(base::ToUtf16(category.name)).c_str());
    }
    RECT picker{};
    ::GetWindowRect(Item(IDC_INFO_CATEGORY), &picker);
    const UINT command = static_cast<UINT>(::TrackPopupMenuEx(menu.get(), TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                                              picker.left, picker.bottom, Handle(), nullptr));
    if (command == 0 || command > initial_.categories.size()) {
        return;
    }
    const model::CategoryId chosen = initial_.categories[command - 1].id;
    if (chosen == category_) {
        return;
    }
    category_ = chosen;
    ShowCategory();
    OnCategoryChanged();
}

void DownloadInfoDialog::OnCategoryChanged()
{
    UpdateRememberText();
    // A category brings its folder, unless the user chose a folder of their own.
    const model::Category* category = FindCategory(category_);
    if (category == nullptr || folderEdited_) {
        return;
    }
    SetFolder(base::ToUtf16(category->folder));
}

void DownloadInfoDialog::UpdateRememberText()
{
    if (const model::Category* category = FindCategory(category_)) {
        SetText(IDC_INFO_REMEMBER, L"&Always use this folder for " + base::ToUtf16(category->name));
        return;
    }
    SetText(IDC_INFO_REMEMBER, L"&Remember this folder for the category");
}

void DownloadInfoDialog::SetFolder(const std::wstring& folder)
{
    settingText_ = true;
    SetText(IDC_INFO_FOLDER, folder);
    settingText_ = false;
    ShowFolder();
    UpdateFreeSpace();
}

void DownloadInfoDialog::ShowFolder()
{
    SetText(IDC_INFO_FOLDER_PICKER, FriendlyFolder(std::filesystem::path(Text(IDC_INFO_FOLDER)), downloads_));
    ::InvalidateRect(Item(IDC_INFO_FOLDER_PICKER), nullptr, TRUE);
}

void DownloadInfoDialog::ChooseFolder()
{
    // The recent folders, without repeats, then the other ways to choose one.
    const std::wstring current = Text(IDC_INFO_FOLDER);
    std::vector<std::wstring> folders;
    for (const std::string& folder : initial_.recentFolders) {
        const std::wstring wide = base::ToUtf16(folder);
        const bool listed = std::any_of(folders.begin(), folders.end(), [&wide](const auto& f) { return SamePath(f, wide); });
        if (!wide.empty() && !listed && folders.size() < kMenuFolders) {
            folders.push_back(wide);
        }
    }
    UniqueMenu menu(::CreatePopupMenu());
    for (size_t i = 0; i < folders.size(); ++i) {
        const UINT flags = MF_STRING | (SamePath(folders[i], current) ? MF_CHECKED : MF_UNCHECKED);
        ::AppendMenuW(menu.get(), flags, i + 1, MenuText(folders[i]).c_str());
    }
    if (!folders.empty()) {
        ::AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    }
    ::AppendMenuW(menu.get(), MF_STRING, kBrowseCommand, L"&Choose another folder\u2026");
    ::AppendMenuW(menu.get(), MF_STRING, kTypePathCommand, L"&Type a folder path");
    RECT picker{};
    ::GetWindowRect(Item(IDC_INFO_FOLDER_PICKER), &picker);
    const UINT command = static_cast<UINT>(::TrackPopupMenuEx(menu.get(), TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                                              picker.left, picker.bottom, Handle(), nullptr));
    if (command == kBrowseCommand) {
        Browse();
    } else if (command == kTypePathCommand) {
        EditFolder();
    } else if (command >= 1 && command <= folders.size()) {
        folderEdited_ = true;
        SetFolder(folders[command - 1]);
    }
}

void DownloadInfoDialog::EditFolder()
{
    Show(IDC_INFO_FOLDER_PICKER, false);
    Show(IDC_INFO_FOLDER, true);
    ::SetFocus(Item(IDC_INFO_FOLDER));
    Edit_SetSel(Item(IDC_INFO_FOLDER), 0, -1);
}

void DownloadInfoDialog::EndFolderEdit()
{
    if (!::IsWindowVisible(Item(IDC_INFO_FOLDER))) {
        return;
    }
    Show(IDC_INFO_FOLDER, false);
    Show(IDC_INFO_FOLDER_PICKER, true);
    ShowFolder();
    UpdateFreeSpace();
}

void DownloadInfoDialog::UpdateFreeSpace()
{
    SetText(IDC_INFO_FREE, FreeSpaceText(Text(IDC_INFO_FOLDER)));
}

void DownloadInfoDialog::Browse()
{
    if (auto folder = PickFolder(Handle(), Text(IDC_INFO_FOLDER))) {
        folderEdited_ = true;
        SetFolder(*folder);
    }
}

void DownloadInfoDialog::SetCategories(std::vector<model::Category> categories, std::optional<model::CategoryId> select)
{
    if (Handle() == nullptr) {
        return;
    }
    initial_.categories = std::move(categories);
    if (select) {
        category_ = select;
        folderEdited_ = false;
        ShowCategory();
        OnCategoryChanged();
        return;
    }
    if (FindCategory(category_) == nullptr) {
        category_ = initial_.categories.empty() ? std::nullopt : std::optional(initial_.categories.front().id);
    }
    ShowCategory();
    UpdateRememberText();
}

void DownloadInfoDialog::ShowFileType(const std::wstring& fileName)
{
    // Updates come several times a second; the tile and type change only with the extension.
    const std::string name = base::ToUtf8(fileName);
    const std::wstring extension = base::ToUtf16(core::FileExtension(name));
    if (extension == tileExtension_) {
        return;
    }
    tileExtension_ = extension;
    FileTile tile;
    tile.badge = FileBadgeGlyph(KindOfFile(name, initial_.categories));
    tile.label = extension.substr(0, 4);
    std::transform(tile.label.begin(), tile.label.end(), tile.label.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    surface_.SetFileTile(IDC_INFO_ICON, std::move(tile));
    SetText(IDC_INFO_TYPE, FileTypeName(extension));
    Show(IDC_INFO_TYPE, !Text(IDC_INFO_TYPE).empty());
    surface_.ArrangeRow({IDC_INFO_SIZE, IDC_INFO_TYPE, IDC_INFO_HOST}, kTagGap);
}

void DownloadInfoDialog::ShowDetails(const std::wstring& size, const std::wstring& note, bool failed)
{
    if (size == shownSize_ && note == shownNote_ && failed == shownFailed_) {
        return;
    }
    shownSize_ = size;
    shownNote_ = note;
    if (failed != shownFailed_) {
        surface_.SetTextRole(IDC_INFO_HOST, failed ? TextRole::Error : TextRole::Caption);
        shownFailed_ = failed;
    }
    SetText(IDC_INFO_SIZE, size);
    Show(IDC_INFO_SIZE, !size.empty());
    SetText(IDC_INFO_HOST, note);
    surface_.ArrangeRow({IDC_INFO_SIZE, IDC_INFO_TYPE, IDC_INFO_HOST}, kTagGap);
}

void DownloadInfoDialog::Update(const core::DownloadView& view)
{
    if (Handle() == nullptr) {
        return;
    }
    const model::Download& d = view.download;
    if (!nameEdited_) {
        settingText_ = true;
        SetText(IDC_INFO_NAME, base::ToUtf16(d.fileName));
        settingText_ = false;
    }
    if (!folderEdited_ && d.categoryId && category_ != d.categoryId && FindCategory(d.categoryId) != nullptr) {
        category_ = d.categoryId;
        ShowCategory();
        UpdateRememberText();
        SetFolder(base::ToUtf16(d.saveDir));
    }

    std::wstring size;
    if (d.totalSize) {
        size = FormatSize(*d.totalSize);
    } else if (view.stream && view.stream->live) {
        size = L"live, " + FormatRecordedLength(view.stream->recordedUs) + L" so far";
    } else if (view.stream && view.stream->estimatedBytes) {
        size = L"about " + FormatSize(*view.stream->estimatedBytes);
    } else {
        size = view.receivedBytes > 0 ? L"size unknown" : L"checking\u2026";
    }
    const bool failed = d.status == model::DownloadStatus::Failed && d.lastResult.has_value();
    ShowDetails(size, failed ? base::ToUtf16(*d.lastResult) : source_, failed);
    ShowFileType(base::ToUtf16(d.fileName));
}

bool DownloadInfoDialog::Finish(Action action)
{
    if (action != Action::Cancel) {
        const std::wstring name = Text(IDC_INFO_NAME);
        const std::wstring folder = Text(IDC_INFO_FOLDER);
        if (name.empty()) {
            Warn(L"Enter a file name.");
            return false;
        }
        if (!std::filesystem::path(folder).is_absolute()) {
            Warn(L"Choose a folder with a full path, such as C:\\Downloads.");
            return false;
        }
        choice_.fileName = base::ToUtf8(name);
        choice_.saveDir = base::ToUtf8(folder);
        choice_.category = SelectedCategory();
        choice_.description = base::ToUtf8(Text(IDC_INFO_DESCRIPTION));
        choice_.rememberFolder = Checked(IDC_INFO_REMEMBER);
    }
    choice_.action = action;
    Close(action == Action::Cancel ? IDCANCEL : IDOK);
    return true;
}

bool DownloadInfoDialog::OnCommand(int id, int code, HWND control)
{
    switch (id) {
    case IDC_INFO_START:
        Finish(Action::Start);
        return true;
    case IDC_INFO_LATER:
        Finish(Action::Later);
        return true;
    case IDCANCEL:
        Finish(Action::Cancel);
        return true;
    case IDC_INFO_CATEGORY:
        if (code == BN_CLICKED) {
            ChooseCategory();
        }
        return true;
    case IDC_INFO_FOLDER_PICKER:
        if (code == BN_CLICKED) {
            // The arrow lists folders; the folder's name opens it for typing, as in an address bar.
            if (surface_.MenuRequested(IDC_INFO_FOLDER_PICKER)) {
                ChooseFolder();
            } else {
                EditFolder();
            }
        }
        return true;
    case IDC_INFO_BROWSE:
        Browse();
        return true;
    case IDC_INFO_COPY:
        if (!WriteClipboardText(Handle(), Text(IDC_INFO_URL))) {
            Warn(L"The address could not be copied: another program is using the clipboard.");
        }
        return true;
    case IDC_INFO_ADD_DESCRIPTION:
        // The description box takes the address line's place.
        Show(IDC_INFO_LINK_ICON, false);
        Show(IDC_INFO_URL, false);
        Show(IDC_INFO_COPY, false);
        Show(IDC_INFO_ADD_DESCRIPTION, false);
        Show(IDC_INFO_DESCRIPTION, true);
        ::SetFocus(Item(IDC_INFO_DESCRIPTION));
        return true;
    case IDC_INFO_ADD_CATEGORY:
        if (addCategory_) {
            if (auto category = addCategory_(Handle())) {
                initial_.categories.push_back(*category);
                SetCategories(initial_.categories, category->id);
            }
        }
        return true;
    case IDC_INFO_NAME:
        if (code == EN_CHANGE && !settingText_) {
            nameEdited_ = true;
            ShowFileType(Text(IDC_INFO_NAME));
        }
        return true;
    case IDC_INFO_FOLDER:
        if (code == EN_CHANGE && !settingText_) {
            folderEdited_ = true;
            ::PostMessageW(Handle(), kFolderChangedMessage, 0, 0);
        } else if (code == EN_KILLFOCUS) {
            EndFolderEdit();
        }
        return true;
    default:
        return Dialog::OnCommand(id, code, control);
    }
}

bool DownloadInfoDialog::OnMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
{
    (void)wParam;
    (void)lParam;
    if (message != kFolderChangedMessage) {
        return false;
    }
    UpdateFreeSpace();
    result = 0;
    return true;
}

}  // namespace ldm::ui
