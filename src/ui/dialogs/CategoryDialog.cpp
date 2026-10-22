#include "ui/dialogs/CategoryDialog.h"

#include "base/Utf.h"
#include "resource.h"
#include "ui/FileDialogs.h"

#include <commctrl.h>
#include <shellapi.h>

#include <filesystem>

namespace ldm::ui {

namespace {

// Extensions are stored lower case, space separated, without dots (checklist W-03).
std::string NormalizeExtensions(const std::wstring& text)
{
    std::string result;
    std::wstring word;
    const auto flush = [&] {
        while (!word.empty() && word.front() == L'.') {
            word.erase(word.begin());
        }
        if (!word.empty()) {
            std::string utf8 = base::ToUtf8(word);
            for (char& c : utf8) {
                c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            }
            result += (result.empty() ? "" : " ") + utf8;
        }
        word.clear();
    };
    for (const wchar_t c : text) {
        if (c == L' ' || c == L',' || c == L';' || c == L'\r' || c == L'\n' || c == L'\t') {
            flush();
        } else {
            word.push_back(c);
        }
    }
    flush();
    return result;
}

}  // namespace

CategoryDialog::CategoryDialog(const Theme& theme, model::Category category)
    : Dialog(IDD_CATEGORY, theme), category_(std::move(category))
{
}

std::optional<model::Category> CategoryDialog::Run(HWND owner)
{
    RunModal(owner);
    return result_;
}

bool CategoryDialog::OnInit()
{
    SetText(IDC_CAT_NAME, base::ToUtf16(category_.name));
    SetText(IDC_CAT_EXTENSIONS, base::ToUtf16(category_.extensions));
    SetText(IDC_CAT_SITES, base::ToUtf16(category_.sites));
    SetText(IDC_CAT_FOLDER, base::ToUtf16(category_.folder));
    SetChecked(IDC_CAT_REMEMBER, category_.rememberLastFolder);
    if (category_.builtin) {
        ::SendMessageW(Item(IDC_CAT_NAME), EM_SETREADONLY, TRUE, 0);  // built-in names stay
    }
    ::SetWindowTextW(Handle(), category_.id.value == 0 ? L"Add category" : L"Category properties");
    return true;
}

bool CategoryDialog::OnCommand(int id, int code, HWND control)
{
    if (id == IDC_CAT_BROWSE) {
        if (auto folder = PickFolder(Handle(), Text(IDC_CAT_FOLDER))) {
            SetText(IDC_CAT_FOLDER, *folder);
        }
        return true;
    }
    if (id == IDOK) {
        model::Category category = category_;
        category.name = base::ToUtf8(Text(IDC_CAT_NAME));
        category.extensions = NormalizeExtensions(Text(IDC_CAT_EXTENSIONS));
        category.sites = base::ToUtf8(Text(IDC_CAT_SITES));
        category.folder = base::ToUtf8(Text(IDC_CAT_FOLDER));
        category.rememberLastFolder = Checked(IDC_CAT_REMEMBER);
        if (category.name.empty()) {
            Warn(L"Enter a name for the category.");
            return true;
        }
        if (!std::filesystem::path(Text(IDC_CAT_FOLDER)).is_absolute()) {
            Warn(L"Choose a folder with a full path, such as C:\\Downloads\\Music.");
            return true;
        }
        result_ = std::move(category);
        Close(IDOK);
        return true;
    }
    return Dialog::OnCommand(id, code, control);
}

SpeedLimitDialog::SpeedLimitDialog(const Theme& theme, Choice initial) : Dialog(IDD_SPEED_LIMIT, theme), initial_(initial) {}

std::optional<SpeedLimitDialog::Choice> SpeedLimitDialog::Run(HWND owner)
{
    RunModal(owner);
    return result_;
}

bool SpeedLimitDialog::OnInit()
{
    SetNumber(IDC_LIMIT_VALUE, initial_.kilobytesPerSecond);
    SetChecked(IDC_LIMIT_STARTUP, initial_.onStartup);
    return true;
}

bool SpeedLimitDialog::OnCommand(int id, int code, HWND control)
{
    if (id == IDOK) {
        const auto value = Number(IDC_LIMIT_VALUE);
        if (!value || *value < 1 || *value > 10'000'000) {
            Warn(L"Enter a speed between 1 and 10000000 KB/sec.");
            return true;
        }
        result_ = Choice{*value, Checked(IDC_LIMIT_STARTUP)};
        Close(IDOK);
        return true;
    }
    return Dialog::OnCommand(id, code, control);
}

AboutDialog::AboutDialog(const Theme& theme) : Dialog(IDD_ABOUT, theme) {}

bool AboutDialog::OnInit()
{
    SetText(IDC_ABOUT_VERSION, L"Version " LDM_VERSION_WIDE);
    return true;
}

bool AboutDialog::OnNotify(const NMHDR& header, LRESULT& result)
{
    if (header.idFrom == IDC_ABOUT_LINK && (header.code == NM_CLICK || header.code == NM_RETURN)) {
        const auto& link = reinterpret_cast<const NMLINK&>(header);
        const std::wstring url = link.item.szUrl;
        if (url.rfind(L"https://", 0) == 0) {
            ::ShellExecuteW(Handle(), L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        result = 0;
        return true;
    }
    return false;
}

}  // namespace ldm::ui
