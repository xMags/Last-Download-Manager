#include "ui/dialogs/DownloadInfoDialog.h"

#include "base/Utf.h"
#include "core/FileNames.h"
#include "resource.h"
#include "ui/Format.h"
#include "ui/FileDialogs.h"

#include <shellapi.h>
#include <windowsx.h>

#include <filesystem>

namespace ldm::ui {

DownloadInfoDialog::DownloadInfoDialog(const Theme& theme, Initial initial,
                                       std::function<std::optional<model::Category>(HWND)> addCategory)
    : Dialog(IDD_DOWNLOAD_INFO, theme), initial_(std::move(initial)), addCategory_(std::move(addCategory))
{
}

DownloadInfoDialog::Choice DownloadInfoDialog::Run(HWND owner)
{
    RunModal(owner);
    return choice_;
}

bool DownloadInfoDialog::OnInit()
{
    settingText_ = true;
    SetText(IDC_INFO_URL, base::ToUtf16(initial_.url));
    SetText(IDC_INFO_NAME, base::ToUtf16(initial_.fileName));
    const HWND folders = Item(IDC_INFO_FOLDER);
    for (const std::string& folder : initial_.recentFolders) {
        ComboBox_AddString(folders, base::ToUtf16(folder).c_str());
    }
    SetText(IDC_INFO_FOLDER, base::ToUtf16(initial_.saveDir));
    settingText_ = false;
    FillCategories();
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
}

void DownloadInfoDialog::Browse()
{
    if (auto folder = PickFolder(Handle(), Text(IDC_INFO_FOLDER))) {
        SetText(IDC_INFO_FOLDER, *folder);
        folderEdited_ = true;
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
    }
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
    if (!folderEdited_ && d.categoryId && SelectedCategory() != d.categoryId) {
        for (int i = 0; i < ComboBox_GetCount(Item(IDC_INFO_CATEGORY)); ++i) {
            if (ComboBox_GetItemData(Item(IDC_INFO_CATEGORY), i) == d.categoryId->value) {
                ComboBox_SetCurSel(Item(IDC_INFO_CATEGORY), i);
            }
        }
        SetText(IDC_INFO_FOLDER, base::ToUtf16(d.saveDir));
    }
    settingText_ = false;

    std::wstring size = L"Size: ";
    size += d.totalSize ? FormatSize(*d.totalSize) : (view.receivedBytes > 0 ? L"unknown" : L"checking...");
    if (d.status == model::DownloadStatus::Failed && d.lastResult) {
        size = base::ToUtf16(*d.lastResult);
    }
    SetText(IDC_INFO_SIZE, size);
    if (d.contentType) {
        SetText(IDC_INFO_TYPE, L"Type: " + base::ToUtf16(*d.contentType));
    }
    SHFILEINFOW info{};
    const std::wstring probe = L"file." + base::ToUtf16(core::FileExtension(d.fileName));
    if (::SHGetFileInfoW(probe.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                         SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | SHGFI_LARGEICON) != 0) {
        fileIcon_.reset(info.hIcon);
        ::SendMessageW(Item(IDC_INFO_ICON), STM_SETICON, reinterpret_cast<WPARAM>(fileIcon_.get()), 0);
    }
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
        }
        return true;
    case IDC_INFO_FOLDER:
        if ((code == CBN_EDITCHANGE || code == CBN_SELCHANGE) && !settingText_) {
            folderEdited_ = true;
        }
        return true;
    default:
        return Dialog::OnCommand(id, code, control);
    }
}

}  // namespace ldm::ui
