#include "ui/dialogs/SiteSignInDialog.h"

#include "base/Utf.h"
#include "core/SiteList.h"
#include "resource.h"

#include <commctrl.h>

#include <algorithm>
#include <sstream>
#include <vector>

namespace ldm::ui {

namespace {

std::vector<std::string> Keys(const std::string& setting)
{
    std::vector<std::string> keys;
    std::istringstream words(setting);
    for (std::string key; words >> key;) {
        keys.push_back(key);
    }
    return keys;
}

}  // namespace

std::wstring SignInSummary(const std::string& setting)
{
    const auto keys = Keys(setting);
    std::wstring summary;
    for (const core::SignInSite& site : core::SignInSites()) {
        if (std::find(keys.begin(), keys.end(), site.key) != keys.end()) {
            summary += (summary.empty() ? L"" : L", ") + base::ToUtf16(site.name);
        }
    }
    return summary.empty() ? std::wstring(L"(none)") : summary;
}

SiteSignInDialog::SiteSignInDialog(const Theme& theme, std::string setting)
    : Dialog(IDD_SITE_SIGNIN, theme), setting_(std::move(setting))
{
}

std::optional<std::string> SiteSignInDialog::Run(HWND owner)
{
    RunModal(owner);
    return std::move(result_);
}

bool SiteSignInDialog::OnInit()
{
    const HWND list = Item(IDC_SI_LIST);
    ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    if (CurrentTheme().IsDark()) {
        ListView_SetBkColor(list, CurrentTheme().Colors().window);
        ListView_SetTextBkColor(list, CurrentTheme().Colors().window);
        ListView_SetTextColor(list, CurrentTheme().Colors().text);
    }
    RECT client{};
    ::GetClientRect(list, &client);
    LVCOLUMNW column{};
    column.mask = LVCF_WIDTH;
    column.cx = client.right - client.left - ::GetSystemMetrics(SM_CXVSCROLL);
    ListView_InsertColumn(list, 0, &column);
    const auto keys = Keys(setting_);
    const auto& sites = core::SignInSites();
    for (size_t i = 0; i < sites.size(); ++i) {
        std::wstring text = base::ToUtf16(sites[i].name);
        if (sites[i].flagsAccounts) {
            text += L" (see the warning below)";
        }
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = text.data();
        item.lParam = static_cast<LPARAM>(i);
        const int inserted = ListView_InsertItem(list, &item);
        const bool on = std::find(keys.begin(), keys.end(), sites[i].key) != keys.end();
        ListView_SetCheckState(list, inserted, on ? TRUE : FALSE);
    }
    return true;
}

bool SiteSignInDialog::OnCommand(int id, int code, HWND control)
{
    if (id != IDOK) {
        return Dialog::OnCommand(id, code, control);
    }
    const HWND list = Item(IDC_SI_LIST);
    const auto& sites = core::SignInSites();
    std::string setting;
    const int count = ListView_GetItemCount(list);
    for (int i = 0; i < count; ++i) {
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = i;
        if (!ListView_GetCheckState(list, i) || !ListView_GetItem(list, &item) ||
            static_cast<size_t>(item.lParam) >= sites.size()) {
            continue;
        }
        setting += (setting.empty() ? "" : " ") + std::string(sites[static_cast<size_t>(item.lParam)].key);
    }
    result_ = std::move(setting);
    Close(IDOK);
    return true;
}

}  // namespace ldm::ui
