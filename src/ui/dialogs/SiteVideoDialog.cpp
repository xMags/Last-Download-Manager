#include "ui/dialogs/SiteVideoDialog.h"

#include "base/Utf.h"
#include "core/SiteList.h"
#include "resource.h"

#include <utility>

namespace ldm::ui {

namespace {

// The list's lines from the panel's menu: submenus become their lines with the submenu's name in
// front ("Video only: 1080p H.264"), notices and the panel's own commands are left out.
void Flatten(const std::vector<PanelMenuItem>& items, const std::wstring& prefix,
             std::vector<std::pair<std::wstring, PanelAction>>& lines)
{
    for (const PanelMenuItem& item : items) {
        if (!item.children.empty()) {
            Flatten(item.children, prefix + item.text + L": ", lines);
            continue;
        }
        if (!item.action || (item.action->kind != PanelAction::Kind::DownloadStream &&
                             item.action->kind != PanelAction::Kind::DownloadSubtitles)) {
            continue;
        }
        lines.emplace_back(prefix + item.text, *item.action);
    }
}

}  // namespace

SiteVideoDialog::SiteVideoDialog(const Theme& theme, browser::SiteRequest request, std::int64_t lookupId, Hooks hooks)
    : Dialog(IDD_SITE_VIDEO, theme), request_(std::move(request)), lookupId_(lookupId), hooks_(std::move(hooks))
{
    // What the panel would send for a video on that page: the page as the referrer.
    tab_.pageUrl = request_.url;
    tab_.pageTitle = request_.pageTitle;
    tab_.incognito = request_.incognito;
    tab_.userAgent = request_.userAgent;
    item_.key = 1;
    item_.kind = browser::MediaKind::Dash;
    item_.siteVideo = true;
    item_.state = MediaItem::State::Fetching;
    item_.lookupId = lookupId_;
    // The download's page is the address the resolver is asked about, as LDM checks it, so a
    // refresh asks about the same page.
    const auto page = core::CheckSitePage(request_.url);
    item_.url = page ? page->address : request_.url;
    item_.finalUrl = item_.url;
}

std::optional<StreamCapture> SiteVideoDialog::Run(HWND owner)
{
    RunModal(owner);
    if (pending_ && hooks_.cancelLookup) {
        hooks_.cancelLookup(lookupId_);  // closed while the resolver was still at it
    }
    pending_ = false;
    return std::move(result_);
}

bool SiteVideoDialog::OnInit()
{
    SetText(IDC_SV_PAGE, base::ToUtf16(request_.url));
    SetText(IDC_SV_STATUS, L"Looking for videos on this page...");
    Enable(IDC_SV_LIST, false);
    Enable(IDOK, false);
    // The list's sizes line up at the right (the panel's menu separates them with a tab).
    const int stops[] = {200};
    ::SendMessageW(Item(IDC_SV_LIST), LB_SETTABSTOPS, 1, reinterpret_cast<LPARAM>(stops));
    pending_ = true;
    if (hooks_.lookUp) {
        hooks_.lookUp(core::LookUpSiteVideoCommand{lookupId_, item_.url});
    }
    return true;
}

bool SiteVideoDialog::OnSiteLookedUp(const core::SiteLookupFinished& finished)
{
    if (!pending_ || finished.lookupId != lookupId_) {
        return false;
    }
    pending_ = false;
    if (finished.playlist) {
        playlist_ = finished.playlist;  // its own window lists them
        Close(IDOK);
        return true;
    }
    FillSiteItem(item_, finished);
    ShowOffer();
    return true;
}

void SiteVideoDialog::ShowOffer()
{
    HWND list = Item(IDC_SV_LIST);
    ::SendMessageW(list, LB_RESETCONTENT, 0, 0);
    actions_.clear();
    if (item_.state != MediaItem::State::Ready) {
        SetText(IDC_SV_STATUS, base::ToUtf16(item_.failure.empty() ? std::string("No video was found on this page.") : item_.failure));
        return;
    }
    std::vector<std::pair<std::wstring, PanelAction>> lines;
    Flatten(BuildPanelMenu({&item_}, false), L"", lines);
    if (lines.empty()) {
        SetText(IDC_SV_STATUS, L"This page offers nothing LDM can download.");
        return;
    }
    for (auto& [text, action] : lines) {
        ::SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        actions_.push_back(action);
    }
    const std::wstring title = item_.siteTitle.empty() ? std::wstring(L"A video") : base::ToUtf16(item_.siteTitle);
    SetText(IDC_SV_STATUS, title + L". Pick a quality:");
    ::SendMessageW(list, LB_SETCURSEL, 0, 0);
    Enable(IDC_SV_LIST, true);
    Enable(IDOK, true);
    ::SetFocus(list);
}

void SiteVideoDialog::Choose()
{
    const LRESULT selected = ::SendMessageW(Item(IDC_SV_LIST), LB_GETCURSEL, 0, 0);
    if (selected == LB_ERR || static_cast<size_t>(selected) >= actions_.size()) {
        return;
    }
    auto capture = BuildStreamCapture(tab_, item_, actions_[static_cast<size_t>(selected)]);
    if (!capture) {
        Warn(base::ToUtf16(capture.GetError().message));
        return;
    }
    result_ = std::move(capture).Value();
    Close(IDOK);
}

bool SiteVideoDialog::OnCommand(int id, int code, HWND control)
{
    if (id == IDOK || (id == IDC_SV_LIST && code == LBN_DBLCLK)) {
        Choose();
        return true;
    }
    return Dialog::OnCommand(id, code, control);
}

}  // namespace ldm::ui
