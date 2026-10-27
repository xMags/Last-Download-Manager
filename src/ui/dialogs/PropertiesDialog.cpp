#include "ui/dialogs/PropertiesDialog.h"

#include "base/Utf.h"
#include "core/FileNames.h"
#include "core/Url.h"
#include "resource.h"
#include "ui/FileDialogs.h"
#include "ui/Format.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <filesystem>

namespace ldm::ui {

namespace {

// A multi-line read-only field keeps its scroll bar only while its text is longer than the field;
// a greyed-out bar beside one short line looks broken.
void ShowScrollBarWhenNeeded(HWND edit)
{
    const HDC dc = ::GetDC(edit);
    if (dc == nullptr) {
        return;
    }
    const auto font = reinterpret_cast<HFONT>(::SendMessageW(edit, WM_GETFONT, 0, 0));  // WM_GETFONT returns the HFONT
    const HGDIOBJ previous = ::SelectObject(dc, font != nullptr ? static_cast<HGDIOBJ>(font) : ::GetStockObject(DEFAULT_GUI_FONT));
    TEXTMETRICW metrics{};
    const BOOL measured = ::GetTextMetricsW(dc, &metrics);
    ::SelectObject(dc, previous);
    ::ReleaseDC(edit, dc);
    RECT client{};
    if (measured == FALSE || metrics.tmHeight <= 0 || ::GetClientRect(edit, &client) == FALSE) {
        return;
    }
    const int visibleLines = std::max(1, static_cast<int>(client.bottom - client.top) / static_cast<int>(metrics.tmHeight));
    ::ShowScrollBar(edit, SB_VERT, Edit_GetLineCount(edit) > visibleLines ? TRUE : FALSE);
}

}  // namespace

PropertiesDialog::PropertiesDialog(const Theme& theme, core::DownloadView view, Callbacks callbacks)
    : Dialog(IDD_PROPERTIES, theme), view_(std::move(view)), callbacks_(std::move(callbacks))
{
}

std::optional<core::UpdateDownloadCommand> PropertiesDialog::Run(HWND owner)
{
    RunModal(owner);
    return result_;
}

bool PropertiesDialog::OnInit()
{
    const model::Download& d = view_.download;
    SetText(IDC_PROP_NAME, base::ToUtf16(d.fileName));
    SHFILEINFOW info{};
    const std::wstring probe = L"file." + base::ToUtf16(core::FileExtension(d.fileName));
    if (::SHGetFileInfoW(probe.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                         SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | SHGFI_LARGEICON | SHGFI_TYPENAME) != 0) {
        icon_.reset(info.hIcon);
        ::SendMessageW(Item(IDC_PROP_ICON), STM_SETICON, reinterpret_cast<WPARAM>(icon_.get()), 0);
    }
    std::wstring type = info.szTypeName;
    if (d.contentType) {
        type += (type.empty() ? L"" : L" (") + base::ToUtf16(*d.contentType) + (type.empty() ? L"" : L")");
    }
    SetText(IDC_PROP_TYPE, type);
    SetText(IDC_PROP_STATUS, FormatStatus(view_));
    std::wstring size = d.totalSize ? FormatSize(*d.totalSize) : L"Unknown";
    if (d.totalSize && *d.totalSize >= 1024) {
        size += L" (" + std::to_wstring(*d.totalSize) + L" bytes)";
    }
    SetText(IDC_PROP_SIZE, size);
    SetText(IDC_PROP_SAVETO, (std::filesystem::path(base::ToUtf16(d.saveDir)) / base::ToUtf16(d.fileName)).wstring());
    SetText(IDC_PROP_URL, base::ToUtf16(d.url));
    SetText(IDC_PROP_DESCRIPTION, base::ToUtf16(d.description));
    SetText(IDC_PROP_REFERER, base::ToUtf16(d.referer.value_or("")));
    SetText(IDC_PROP_PARENT, base::ToUtf16(d.parentPage.value_or("")));
    SetText(IDC_PROP_USER, base::ToUtf16(d.loginUser.value_or("")));
    if (view_.hasPassword) {
        Edit_SetCueBannerText(Item(IDC_PROP_PASSWORD), L"(unchanged)");
    }
    SetText(IDC_PROP_LASTTRY, FormatDateTime(d.lastTryAtMs));
    SetText(IDC_PROP_RESULT, base::ToUtf16(d.lastResult.value_or(d.status == model::DownloadStatus::Completed ? "Complete" : "")));
    ShowScrollBarWhenNeeded(Item(IDC_PROP_RESULT));
    const bool running = d.status == model::DownloadStatus::Downloading || d.status == model::DownloadStatus::Stopping;
    Enable(IDC_PROP_OPEN, d.status == model::DownloadStatus::Completed);
    Enable(IDC_PROP_MOVE, !running && d.status != model::DownloadStatus::Finalizing);
    // A running transfer keeps its address; the change applies from the next start (checklist C-02).
    return true;
}

bool PropertiesDialog::OnCommand(int id, int code, HWND control)
{
    switch (id) {
    case IDC_PROP_OPEN:
        if (callbacks_.open) {
            callbacks_.open(Handle());
        }
        return true;
    case IDC_PROP_MOVE:
        if (callbacks_.move) {
            callbacks_.move(Handle());
            Close(IDCANCEL);
        }
        return true;
    case IDOK: {
        const model::Download& d = view_.download;
        core::UpdateDownloadCommand update;
        update.id = d.id;
        bool changed = false;
        const std::string url = base::ToUtf8(Text(IDC_PROP_URL));
        if (url != d.url) {
            if (!core::ParseUrl(url)) {
                Warn(L"Enter a valid address that starts with http://, https://, ftp:// or ftps://.");
                return true;
            }
            update.url = url;
            changed = true;
        }
        const std::string description = base::ToUtf8(Text(IDC_PROP_DESCRIPTION));
        if (description != d.description) {
            update.description = description;
            changed = true;
        }
        const std::string referer = base::ToUtf8(Text(IDC_PROP_REFERER));
        if (referer != d.referer.value_or("")) {
            update.referer = referer;
            changed = true;
        }
        const std::string user = base::ToUtf8(Text(IDC_PROP_USER));
        if (user != d.loginUser.value_or("")) {
            update.loginUser = user;
            changed = true;
        }
        const std::string password = base::ToUtf8(Text(IDC_PROP_PASSWORD));
        if (!password.empty()) {
            update.loginPassword = password;
            changed = true;
        }
        if (changed) {
            result_ = std::move(update);
        }
        Close(IDOK);
        return true;
    }
    default:
        return Dialog::OnCommand(id, code, control);
    }
}

MoveDialog::MoveDialog(const Theme& theme, std::wstring folder, std::wstring fileName, std::vector<std::string> recentFolders)
    : Dialog(IDD_MOVE, theme), folder_(std::move(folder)), fileName_(std::move(fileName)), recentFolders_(std::move(recentFolders))
{
}

std::optional<MoveDialog::Choice> MoveDialog::Run(HWND owner)
{
    RunModal(owner);
    return result_;
}

bool MoveDialog::OnInit()
{
    AlignWithField(IDC_MOVE_BROWSE, IDC_MOVE_FOLDER);
    for (const std::string& folder : recentFolders_) {
        ComboBox_AddString(Item(IDC_MOVE_FOLDER), base::ToUtf16(folder).c_str());
    }
    SetText(IDC_MOVE_FOLDER, folder_);
    SetText(IDC_MOVE_NAME, fileName_);
    // Select the name without its extension, as Explorer does when renaming.
    const size_t dot = fileName_.rfind(L'.');
    ::SetFocus(Item(IDC_MOVE_NAME));
    Edit_SetSel(Item(IDC_MOVE_NAME), 0, dot == std::wstring::npos || dot == 0 ? -1 : static_cast<int>(dot));
    return false;
}

bool MoveDialog::OnCommand(int id, int code, HWND control)
{
    if (id == IDC_MOVE_BROWSE) {
        if (auto folder = PickFolder(Handle(), Text(IDC_MOVE_FOLDER))) {
            SetText(IDC_MOVE_FOLDER, *folder);
        }
        return true;
    }
    if (id == IDOK) {
        Choice choice{Text(IDC_MOVE_FOLDER), Text(IDC_MOVE_NAME)};
        if (choice.fileName.empty() || choice.fileName.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
            Warn(L"Enter a file name without \\ / : * ? \" < > |.");
            return true;
        }
        if (!std::filesystem::path(choice.folder).is_absolute()) {
            Warn(L"Choose a folder with a full path, such as C:\\Downloads.");
            return true;
        }
        result_ = std::move(choice);
        Close(IDOK);
        return true;
    }
    return Dialog::OnCommand(id, code, control);
}

}  // namespace ldm::ui
