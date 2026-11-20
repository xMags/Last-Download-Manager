#include "ui/dialogs/DownloadInfoDialog.h"

#include "base/Utf.h"
#include "core/FileNames.h"
#include "core/Url.h"
#include "resource.h"
#include "ui/Clipboard.h"
#include "ui/Dpi.h"
#include "ui/FileDialogs.h"
#include "ui/FileTypeInfo.h"
#include "ui/Format.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>

namespace ldm::ui {

namespace {

// Dialog units of the footer band, from the template's layout.
constexpr int kFooterDialogUnits = 37;
// Between the tags and the line after them, in dialog units.
constexpr int kTagGap = 3;
// The file icon on the tile, at 96 DPI.
constexpr int kTileIcon96 = 48;
// Posted when the folder box changed, to show the free space of the new folder's drive.
constexpr UINT kFolderChangedMessage = WM_APP + 0x110;
// The icon font's Add glyph, for the new-category button.
constexpr wchar_t kAddGlyph = 0xE710;
// Heights of the drawn text boxes at 96 DPI, matching their template rectangles.
constexpr int kNameField96 = 36;
constexpr int kDescriptionField96 = 32;

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
    surface_.Attach(Handle());
    surface_.SetFooterHeight(kFooterDialogUnits);
    surface_.AddPanel(IDC_INFO_PANEL);
    surface_.AddButton(IDC_INFO_START, ButtonRole::Primary);
    surface_.AddButton(IDC_INFO_LATER, ButtonRole::Secondary);
    surface_.AddButton(IDCANCEL, ButtonRole::Secondary);
    surface_.AddButton(IDC_INFO_ADD_CATEGORY, ButtonRole::Secondary);
    surface_.AddButton(IDC_INFO_BROWSE, ButtonRole::Secondary);
    surface_.SetButtonGlyph(IDC_INFO_ADD_CATEGORY, kAddGlyph);
    surface_.AddButton(IDC_INFO_COPY, ButtonRole::Link);
    surface_.AddButton(IDC_INFO_ADD_DESCRIPTION, ButtonRole::Link);
    surface_.AddChip(IDC_INFO_SIZE);
    surface_.AddChip(IDC_INFO_TYPE);
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
    surface_.AddField(IDC_INFO_DESCRIPTION, kDescriptionField96);
    Edit_SetCueBannerText(Item(IDC_INFO_DESCRIPTION), L"Description");

    settingText_ = true;
    nameEdited_ = initial_.fileNameFixed;
    folderEdited_ = initial_.saveDirFixed;
    AlignWithField(IDC_INFO_ADD_CATEGORY, IDC_INFO_CATEGORY);
    AlignWithField(IDC_INFO_BROWSE, IDC_INFO_FOLDER);
    SetText(IDC_INFO_URL, base::ToUtf16(initial_.url));
    SetText(IDC_INFO_NAME, base::ToUtf16(initial_.fileName));
    const HWND folders = Item(IDC_INFO_FOLDER);
    for (const std::string& folder : initial_.recentFolders) {
        ComboBox_AddString(folders, base::ToUtf16(folder).c_str());
    }
    SetText(IDC_INFO_FOLDER, base::ToUtf16(initial_.saveDir));
    settingText_ = false;
    FillCategories();

    if (const auto url = core::ParseUrl(initial_.url); url && !url->host.empty()) {
        source_ = L"from " + base::ToUtf16(url->host);
    }
    ShowFileType(base::ToUtf16(initial_.fileName));
    ShowDetails(L"checking\u2026", source_, false);
    UpdateFreeSpace();
    UpdateRememberText();
    ::SetFocus(Item(IDC_INFO_START));
    return false;
}

void DownloadInfoDialog::FillCategories()
{
    const HWND combo = Item(IDC_INFO_CATEGORY);
    ComboBox_ResetContent(combo);
    int selected = 0;
    for (const model::Category& category : initial_.categories) {
        const int index = ComboBox_AddString(combo, base::ToUtf16(category.name).c_str());
        ComboBox_SetItemData(combo, index, category.id.value);
        if (initial_.category == category.id) {
            selected = index;
        }
    }
    ComboBox_SetCurSel(combo, selected);
}

std::optional<model::CategoryId> DownloadInfoDialog::SelectedCategory() const
{
    const HWND combo = Item(IDC_INFO_CATEGORY);
    const int index = ComboBox_GetCurSel(combo);
    if (index < 0) {
        return std::nullopt;
    }
    return model::CategoryId{static_cast<std::int64_t>(ComboBox_GetItemData(combo, index))};
}

void DownloadInfoDialog::OnCategoryChanged()
{
    UpdateRememberText();
    // A category brings its folder, unless the user typed a folder of their own.
    const auto id = SelectedCategory();
    if (!id || folderEdited_) {
        return;
    }
    for (const model::Category& category : initial_.categories) {
        if (category.id == *id) {
            settingText_ = true;
            SetText(IDC_INFO_FOLDER, base::ToUtf16(category.folder));
            settingText_ = false;
        }
    }
    UpdateFreeSpace();
}

void DownloadInfoDialog::UpdateRememberText()
{
    const auto id = SelectedCategory();
    for (const model::Category& category : initial_.categories) {
        if (id && category.id == *id) {
            SetText(IDC_INFO_REMEMBER, L"&Always use this folder for " + base::ToUtf16(category.name));
            return;
        }
    }
    SetText(IDC_INFO_REMEMBER, L"&Remember this folder for the category");
}

void DownloadInfoDialog::UpdateFreeSpace()
{
    SetText(IDC_INFO_FREE, FreeSpaceText(Text(IDC_INFO_FOLDER)));
}

void DownloadInfoDialog::Browse()
{
    if (auto folder = PickFolder(Handle(), Text(IDC_INFO_FOLDER))) {
        SetText(IDC_INFO_FOLDER, *folder);
        folderEdited_ = true;
        UpdateFreeSpace();
    }
}

void DownloadInfoDialog::SetCategories(std::vector<model::Category> categories, std::optional<model::CategoryId> select)
{
    if (Handle() == nullptr) {
        return;
    }
    initial_.categories = std::move(categories);
    initial_.category = select ? select : SelectedCategory();
    FillCategories();
    if (select) {
        folderEdited_ = false;
        OnCategoryChanged();
    } else {
        UpdateRememberText();
    }
}

void DownloadInfoDialog::ShowFileType(const std::wstring& fileName)
{
    // Updates come several times a second; the icon and type change only with the extension.
    const std::wstring extension = base::ToUtf16(core::FileExtension(base::ToUtf8(fileName)));
    if (fileIcon_ && extension == fileIconExtension_) {
        return;
    }
    UniqueIcon icon = FileTypeIcon(extension, Scale(kTileIcon96, DpiOf(Handle())));
    // Shown before the old icon is destroyed, so the tile never draws a freed one.
    surface_.SetFileTile(IDC_INFO_ICON, icon.get(), false);
    fileIcon_ = std::move(icon);
    fileIconExtension_ = extension;
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
        surface_.SetTextRole(IDC_INFO_HOST, failed ? TextRole::Error : TextRole::Muted);
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
    settingText_ = true;
    if (!nameEdited_) {
        SetText(IDC_INFO_NAME, base::ToUtf16(d.fileName));
    }
    bool folderChanged = false;
    if (!folderEdited_ && d.categoryId && SelectedCategory() != d.categoryId) {
        for (int i = 0; i < ComboBox_GetCount(Item(IDC_INFO_CATEGORY)); ++i) {
            if (ComboBox_GetItemData(Item(IDC_INFO_CATEGORY), i) == d.categoryId->value) {
                ComboBox_SetCurSel(Item(IDC_INFO_CATEGORY), i);
            }
        }
        SetText(IDC_INFO_FOLDER, base::ToUtf16(d.saveDir));
        folderChanged = true;
    }
    settingText_ = false;
    if (folderChanged) {
        UpdateFreeSpace();
        UpdateRememberText();
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
    case IDC_INFO_CATEGORY:
        if (code == CBN_SELCHANGE) {
            OnCategoryChanged();
        }
        return true;
    case IDC_INFO_NAME:
        if (code == EN_CHANGE && !settingText_) {
            nameEdited_ = true;
            ShowFileType(Text(IDC_INFO_NAME));
        }
        return true;
    case IDC_INFO_FOLDER:
        if ((code == CBN_EDITCHANGE || code == CBN_SELCHANGE) && !settingText_) {
            folderEdited_ = true;
        }
        if (code == CBN_EDITCHANGE || code == CBN_SELCHANGE) {
            // A picked entry reaches the box's text only after CBN_SELCHANGE returns.
            ::PostMessageW(Handle(), kFolderChangedMessage, 0, 0);
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
