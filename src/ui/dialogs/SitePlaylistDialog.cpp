#include "ui/dialogs/SitePlaylistDialog.h"

#include "base/Utf.h"
#include "resource.h"

#include <commctrl.h>
#include <windowsx.h>

#include <array>
#include <cmath>
#include <string>
#include <utility>

namespace ldm::ui {

namespace {

struct QualityOption {
    const wchar_t* text;
    model::SiteQuality quality;
};

constexpr std::array<QualityOption, 7> kQualities{{
    {L"Best quality", model::SiteQuality::Best},
    {L"Up to 2160p (4K)", model::SiteQuality::UpTo2160},
    {L"Up to 1440p", model::SiteQuality::UpTo1440},
    {L"Up to 1080p", model::SiteQuality::UpTo1080},
    {L"Up to 720p", model::SiteQuality::UpTo720},
    {L"Up to 480p", model::SiteQuality::UpTo480},
    {L"Audio only", model::SiteQuality::AudioOnly},
}};

std::wstring Length(const std::optional<double>& seconds)
{
    if (!seconds) {
        return {};
    }
    const auto total = static_cast<long long>(std::llround(*seconds));
    const long long hours = total / 3600;
    const long long minutes = total / 60 % 60;
    const long long rest = total % 60;
    wchar_t text[32] = {};
    if (hours > 0) {
        swprintf_s(text, L"%lld:%02lld:%02lld", hours, minutes, rest);
    } else {
        swprintf_s(text, L"%lld:%02lld", minutes, rest);
    }
    return text;
}

}  // namespace

SitePlaylistDialog::SitePlaylistDialog(const Theme& theme, const media::SitePlaylist& playlist)
    : Dialog(IDD_SITE_PLAYLIST, theme), playlist_(playlist)
{
}

std::optional<SitePlaylistDialog::Choice> SitePlaylistDialog::Run(HWND owner)
{
    RunModal(owner);
    return std::move(result_);
}

bool SitePlaylistDialog::OnInit()
{
    std::wstring title = playlist_.title.empty() ? std::wstring(L"This page") : base::ToUtf16(playlist_.title);
    title += L" lists " + std::to_wstring(playlist_.entries.size()) + (playlist_.entries.size() == 1 ? L" video" : L" videos");
    if (playlist_.truncated) {
        title += L", the first " + std::to_wstring(media::kMaxSitePlaylistEntries) + L" of more";
    }
    SetText(IDC_SP_TITLE, title + L". Check the ones to download.");

    const HWND list = Item(IDC_SP_LIST);
    ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    if (CurrentTheme().IsDark()) {
        ListView_SetBkColor(list, CurrentTheme().Colors().window);
        ListView_SetTextBkColor(list, CurrentTheme().Colors().window);
        ListView_SetTextColor(list, CurrentTheme().Colors().text);
    }
    RECT client{};
    ::GetClientRect(list, &client);
    const int width = client.right - client.left - ::GetSystemMetrics(SM_CXVSCROLL);
    const std::array<std::pair<const wchar_t*, int>, 3> columns{{
        {L"Title", width * 62 / 100}, {L"Length", width * 12 / 100}, {L"List", width * 26 / 100}}};
    for (int column = 0; column < static_cast<int>(columns.size()); ++column) {
        LVCOLUMNW item{};
        item.mask = LVCF_TEXT | LVCF_WIDTH;
        item.pszText = const_cast<wchar_t*>(columns[static_cast<size_t>(column)].first);
        item.cx = columns[static_cast<size_t>(column)].second;
        ListView_InsertColumn(list, column, &item);
    }
    filling_ = true;
    for (size_t i = 0; i < playlist_.entries.size(); ++i) {
        const media::SitePlaylistEntry& entry = playlist_.entries[i];
        std::wstring name = entry.title.empty() ? base::ToUtf16(entry.url) : base::ToUtf16(entry.title);
        if (name.empty()) {
            name = L"Video " + std::to_wstring(i + 1);
        }
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = name.data();
        item.lParam = static_cast<LPARAM>(i);
        const int inserted = ListView_InsertItem(list, &item);
        std::wstring length = Length(entry.seconds);
        ListView_SetItemText(list, inserted, 1, length.data());
        std::wstring group = base::ToUtf16(entry.group);
        ListView_SetItemText(list, inserted, 2, group.data());
        ListView_SetCheckState(list, inserted, TRUE);
    }
    filling_ = false;

    const HWND quality = Item(IDC_SP_QUALITY);
    for (const QualityOption& option : kQualities) {
        ComboBox_AddString(quality, option.text);
    }
    ComboBox_SetCurSel(quality, 0);
    UpdateCount();
    return true;
}

void SitePlaylistDialog::CheckAll(bool checked)
{
    const HWND list = Item(IDC_SP_LIST);
    const int count = ListView_GetItemCount(list);
    filling_ = true;
    for (int i = 0; i < count; ++i) {
        ListView_SetCheckState(list, i, checked ? TRUE : FALSE);
    }
    filling_ = false;
    UpdateCount();
}

void SitePlaylistDialog::UpdateCount()
{
    const HWND list = Item(IDC_SP_LIST);
    const int count = ListView_GetItemCount(list);
    int checked = 0;
    for (int i = 0; i < count; ++i) {
        checked += ListView_GetCheckState(list, i) ? 1 : 0;
    }
    SetText(IDC_SP_COUNT, std::to_wstring(checked) + L" of " + std::to_wstring(count) + L" checked");
    Enable(IDOK, checked > 0);
}

bool SitePlaylistDialog::OnNotify(const NMHDR& header, LRESULT& result)
{
    if (header.idFrom == IDC_SP_LIST && header.code == LVN_ITEMCHANGED) {
        const auto& change = reinterpret_cast<const NMLISTVIEW&>(header);
        if (!filling_ && (change.uChanged & LVIF_STATE) != 0 &&
            ((change.uOldState ^ change.uNewState) & LVIS_STATEIMAGEMASK) != 0) {
            UpdateCount();
        }
        result = 0;
        return true;
    }
    return false;
}

bool SitePlaylistDialog::OnCommand(int id, int code, HWND control)
{
    switch (id) {
    case IDC_SP_ALL:
        CheckAll(true);
        return true;
    case IDC_SP_NONE:
        CheckAll(false);
        return true;
    case IDOK: {
        const HWND list = Item(IDC_SP_LIST);
        Choice choice;
        const int count = ListView_GetItemCount(list);
        for (int i = 0; i < count; ++i) {
            if (!ListView_GetCheckState(list, i)) {
                continue;
            }
            LVITEMW item{};
            item.mask = LVIF_PARAM;
            item.iItem = i;
            if (ListView_GetItem(list, &item) && static_cast<size_t>(item.lParam) < playlist_.entries.size()) {
                choice.entries.push_back(static_cast<size_t>(item.lParam));
            }
        }
        if (choice.entries.empty()) {
            return true;
        }
        const int selected = ComboBox_GetCurSel(Item(IDC_SP_QUALITY));
        choice.quality = selected >= 0 && static_cast<size_t>(selected) < kQualities.size()
                             ? kQualities[static_cast<size_t>(selected)].quality
                             : model::SiteQuality::Best;
        choice.start = Checked(IDC_SP_START);
        result_ = std::move(choice);
        Close(IDOK);
        return true;
    }
    default:
        return Dialog::OnCommand(id, code, control);
    }
}

}  // namespace ldm::ui
