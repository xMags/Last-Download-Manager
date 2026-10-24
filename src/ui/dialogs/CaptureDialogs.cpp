#include "ui/dialogs/CaptureDialogs.h"

#include "resource.h"
#include "base/Utf.h"

#include <commctrl.h>

#include <algorithm>
#include <cwctype>

namespace ldm::ui {

namespace {

constexpr size_t kMaxPattern = 2048;
constexpr int kMaxMinimumKilobytes = 10'000'000;  // the setting's range (Settings.cpp)

constexpr const wchar_t* kSpaces = L" \t\r\n";

// The words of `text` joined by single spaces.
std::string Words(const std::wstring& text)
{
    std::wstring joined;
    size_t at = 0;
    while (at < text.size()) {
        const size_t start = text.find_first_not_of(kSpaces, at);
        if (start == std::wstring::npos) {
            break;
        }
        const size_t end = std::min(text.find_first_of(kSpaces, start), text.size());
        joined += (joined.empty() ? L"" : L" ") + text.substr(start, end - start);
        at = end;
    }
    return base::ToUtf8(joined);
}

}  // namespace

std::optional<std::wstring> CleanAddressPattern(const std::wstring& text)
{
    const size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return std::nullopt;
    }
    const size_t last = text.find_last_not_of(L" \t\r\n");
    std::wstring pattern = text.substr(first, last - first + 1);
    const bool clean = std::none_of(pattern.begin(), pattern.end(), [](wchar_t c) { return c <= L' ' || c == 0x7F; });
    if (!clean || pattern.size() > kMaxPattern) {
        return std::nullopt;
    }
    return pattern;
}

AddressExceptionsDialog::AddressExceptionsDialog(const Theme& theme, std::vector<std::wstring> addresses)
    : Dialog(IDD_ADDRESS_EXCEPTIONS, theme), addresses_(std::move(addresses))
{
}

std::optional<std::vector<std::wstring>> AddressExceptionsDialog::Run(HWND owner)
{
    RunModal(owner);
    return result_;
}

bool AddressExceptionsDialog::OnInit()
{
    const HWND list = Item(IDC_EXC_LIST);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    if (CurrentTheme().IsDark()) {
        ListView_SetBkColor(list, CurrentTheme().Colors().window);
        ListView_SetTextBkColor(list, CurrentTheme().Colors().window);
        ListView_SetTextColor(list, CurrentTheme().Colors().text);
    }
    RECT client{};
    ::GetClientRect(list, &client);
    LVCOLUMNW column{};
    column.mask = LVCF_WIDTH;
    column.cx = client.right - client.left - ::GetSystemMetricsForDpi(SM_CXVSCROLL, ::GetDpiForWindow(list));
    ListView_InsertColumn(list, 0, &column);
    Refresh(addresses_.empty() ? -1 : 0);
    return true;
}

void AddressExceptionsDialog::Refresh(int select)
{
    const HWND list = Item(IDC_EXC_LIST);
    ListView_DeleteAllItems(list);
    for (size_t i = 0; i < addresses_.size(); ++i) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = addresses_[i].data();  // copied by the list view
        ListView_InsertItem(list, &item);
    }
    if (select >= 0 && select < static_cast<int>(addresses_.size())) {
        ListView_SetItemState(list, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, select, FALSE);
    }
    UpdateButtons();
}

void AddressExceptionsDialog::UpdateButtons()
{
    Enable(IDC_EXC_DELETE, ListView_GetSelectedCount(Item(IDC_EXC_LIST)) > 0);
}

bool AddressExceptionsDialog::OnCommand(int id, int code, HWND control)
{
    switch (id) {
    case IDC_EXC_ADD: {
        AddExceptionDialog add(CurrentTheme());
        if (auto address = add.Run(Handle())) {
            const auto existing = std::find(addresses_.begin(), addresses_.end(), *address);
            if (existing != addresses_.end()) {
                Refresh(static_cast<int>(existing - addresses_.begin()));
            } else {
                addresses_.push_back(std::move(*address));
                Refresh(static_cast<int>(addresses_.size()) - 1);
            }
        }
        return true;
    }
    case IDC_EXC_DELETE: {
        const HWND list = Item(IDC_EXC_LIST);
        int first = -1;
        // Remove from the end so earlier indexes stay valid.
        for (int row = ListView_GetItemCount(list) - 1; row >= 0; --row) {
            if (ListView_GetItemState(list, row, LVIS_SELECTED) != 0) {
                addresses_.erase(addresses_.begin() + row);
                first = row;
            }
        }
        Refresh(std::min(first, static_cast<int>(addresses_.size()) - 1));
        return true;
    }
    case IDOK:
        result_ = addresses_;
        Close(IDOK);
        return true;
    default:
        return Dialog::OnCommand(id, code, control);
    }
}

bool AddressExceptionsDialog::OnNotify(const NMHDR& header, LRESULT& result)
{
    (void)result;
    if (header.idFrom == IDC_EXC_LIST && header.code == LVN_ITEMCHANGED) {
        UpdateButtons();
    }
    if (header.idFrom == IDC_EXC_LIST && header.code == LVN_KEYDOWN &&
        reinterpret_cast<const NMLVKEYDOWN&>(header).wVKey == VK_DELETE) {  // header is the start of NMLVKEYDOWN
        ::SendMessageW(Handle(), WM_COMMAND, MAKEWPARAM(IDC_EXC_DELETE, BN_CLICKED), 0);
    }
    return false;
}

AddExceptionDialog::AddExceptionDialog(const Theme& theme) : Dialog(IDD_ADD_EXCEPTION, theme) {}

std::optional<std::wstring> AddExceptionDialog::Run(HWND owner)
{
    RunModal(owner);
    return address_;
}

bool AddExceptionDialog::OnInit()
{
    ::SetFocus(Item(IDC_EXC_ADDRESS));
    return false;
}

bool AddExceptionDialog::OnCommand(int id, int code, HWND control)
{
    if (id != IDOK) {
        return Dialog::OnCommand(id, code, control);
    }
    auto address = CleanAddressPattern(Text(IDC_EXC_ADDRESS));
    if (!address) {
        Warn(L"Enter an address without spaces, such as https://example.com/files/*.");
        return true;
    }
    address_ = std::move(address);
    Close(IDOK);
    return true;
}

ExceptionOfferDialog::ExceptionOfferDialog(const Theme& theme, std::wstring site, std::wstring address, Kind preselected)
    : Dialog(IDD_CAPTURE_EXCEPTION_OFFER, theme), site_(std::move(site)), address_(std::move(address)),
      preselected_(preselected)
{
}

ExceptionOfferDialog::Choice ExceptionOfferDialog::Run(HWND owner)
{
    RunModal(owner);
    return choice_;
}

bool ExceptionOfferDialog::OnInit()
{
    SetText(IDC_OFFER_TEXT, L"You cancelled two downloads from " + site_ +
                                L" that LDM took from the browser. To let the browser keep such downloads, add an "
                                L"exception:");
    SetText(IDC_OFFER_SITE_EDIT, site_);
    SetText(IDC_OFFER_ADDRESS_EDIT, address_);
    SetChecked(preselected_ == Kind::Site ? IDC_OFFER_SITE : IDC_OFFER_ADDRESS, true);
    UpdateFields();
    return true;
}

void ExceptionOfferDialog::UpdateFields()
{
    Enable(IDC_OFFER_SITE_EDIT, Checked(IDC_OFFER_SITE));
    Enable(IDC_OFFER_ADDRESS_EDIT, Checked(IDC_OFFER_ADDRESS));
}

bool ExceptionOfferDialog::OnCommand(int id, int code, HWND control)
{
    if (id == IDC_OFFER_SITE || id == IDC_OFFER_ADDRESS) {
        UpdateFields();
        return true;
    }
    if (id == IDCANCEL) {
        choice_.dontAskAgain = Checked(IDC_OFFER_DONT_ASK);
        return Dialog::OnCommand(id, code, control);
    }
    if (id != IDOK) {
        return Dialog::OnCommand(id, code, control);
    }
    const Kind kind = Checked(IDC_OFFER_SITE) ? Kind::Site : Kind::Address;
    auto value = CleanAddressPattern(Text(kind == Kind::Site ? IDC_OFFER_SITE_EDIT : IDC_OFFER_ADDRESS_EDIT));
    if (!value) {
        Warn(kind == Kind::Site ? L"Enter a site name without spaces, such as files.example.com."
                                : L"Enter an address without spaces, such as https://example.com/files/*.");
        return true;
    }
    choice_.kind = kind;
    choice_.value = std::move(*value);
    choice_.dontAskAgain = Checked(IDC_OFFER_DONT_ASK);
    Close(IDOK);
    return true;
}

CaptureKeysDialog::CaptureKeysDialog(const Theme& theme, model::CaptureKeys keys)
    : Dialog(IDD_CAPTURE_KEYS, theme), keys_(keys)
{
}

std::optional<model::CaptureKeys> CaptureKeysDialog::Run(HWND owner)
{
    RunModal(owner);
    return result_;
}

bool CaptureKeysDialog::OnInit()
{
    SetChecked(IDC_KEYS_PREVENT, keys_.preventEnabled);
    SetChecked(IDC_KEYS_PREVENT_ALT, keys_.preventAlt);
    SetChecked(IDC_KEYS_PREVENT_SHIFT, keys_.preventShift);
    SetChecked(IDC_KEYS_PREVENT_CTRL, keys_.preventCtrl);
    SetChecked(IDC_KEYS_FORCE, keys_.forceEnabled);
    SetChecked(IDC_KEYS_FORCE_ALT, keys_.forceAlt);
    SetChecked(IDC_KEYS_FORCE_SHIFT, keys_.forceShift);
    SetChecked(IDC_KEYS_FORCE_CTRL, keys_.forceCtrl);
    SetChecked(IDC_KEYS_FORCE_INSERT, keys_.forceInsert);
    SetChecked(IDC_KEYS_FORCE_DELETE, keys_.forceDelete);
    SetChecked(IDC_KEYS_LINK_CLICK, keys_.forceNeedsLinkClick);
    SetChecked(IDC_KEYS_SKIP_WEB, keys_.skipWebContent);
    UpdateControls();
    return true;
}

void CaptureKeysDialog::UpdateControls()
{
    const bool prevent = Checked(IDC_KEYS_PREVENT);
    for (const int id : {IDC_KEYS_PREVENT_ALT, IDC_KEYS_PREVENT_SHIFT, IDC_KEYS_PREVENT_CTRL}) {
        Enable(id, prevent);
    }
    const bool force = Checked(IDC_KEYS_FORCE);
    for (const int id : {IDC_KEYS_FORCE_ALT, IDC_KEYS_FORCE_SHIFT, IDC_KEYS_FORCE_CTRL, IDC_KEYS_FORCE_INSERT,
                         IDC_KEYS_FORCE_DELETE, IDC_KEYS_LINK_CLICK, IDC_KEYS_SKIP_WEB}) {
        Enable(id, force);
    }
}

bool CaptureKeysDialog::OnCommand(int id, int code, HWND control)
{
    if (id == IDC_KEYS_PREVENT || id == IDC_KEYS_FORCE) {
        UpdateControls();
        return true;
    }
    if (id != IDOK) {
        return Dialog::OnCommand(id, code, control);
    }
    model::CaptureKeys keys;
    keys.preventEnabled = Checked(IDC_KEYS_PREVENT);
    keys.preventAlt = Checked(IDC_KEYS_PREVENT_ALT);
    keys.preventShift = Checked(IDC_KEYS_PREVENT_SHIFT);
    keys.preventCtrl = Checked(IDC_KEYS_PREVENT_CTRL);
    keys.forceEnabled = Checked(IDC_KEYS_FORCE);
    keys.forceAlt = Checked(IDC_KEYS_FORCE_ALT);
    keys.forceShift = Checked(IDC_KEYS_FORCE_SHIFT);
    keys.forceCtrl = Checked(IDC_KEYS_FORCE_CTRL);
    keys.forceInsert = Checked(IDC_KEYS_FORCE_INSERT);
    keys.forceDelete = Checked(IDC_KEYS_FORCE_DELETE);
    keys.forceNeedsLinkClick = Checked(IDC_KEYS_LINK_CLICK);
    keys.skipWebContent = Checked(IDC_KEYS_SKIP_WEB);
    // A turned-on set with no key would act on every click.
    if (keys.preventEnabled && !keys.preventAlt && !keys.preventShift && !keys.preventCtrl) {
        Warn(L"Choose at least one key that keeps a link in the browser, or clear that option.");
        return true;
    }
    if (keys.forceEnabled && !keys.forceAlt && !keys.forceShift && !keys.forceCtrl && !keys.forceInsert &&
        !keys.forceDelete) {
        Warn(L"Choose at least one key that gives a link to LDM, or clear that option.");
        return true;
    }
    const bool same = keys.preventEnabled && keys.forceEnabled && keys.preventAlt == keys.forceAlt &&
                      keys.preventShift == keys.forceShift && keys.preventCtrl == keys.forceCtrl && !keys.forceInsert &&
                      !keys.forceDelete;
    if (same) {
        Warn(L"The keys that keep a link in the browser and the keys that give it to LDM must differ.");
        return true;
    }
    result_ = keys;
    Close(IDOK);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Browser context menu

BrowserMenuDialog::BrowserMenuDialog(const Theme& theme, BrowserMenus menus)
    : Dialog(IDD_BROWSER_MENU, theme), menus_(menus)
{
}

std::optional<BrowserMenus> BrowserMenuDialog::Run(HWND owner)
{
    RunModal(owner);
    return result_;
}

bool BrowserMenuDialog::OnInit()
{
    SetChecked(IDC_MENU_CHROMIUM_LINK, menus_.chromium.downloadLink);
    SetChecked(IDC_MENU_CHROMIUM_ALL, menus_.chromium.downloadAll);
    SetChecked(IDC_MENU_FIREFOX_LINK, menus_.firefox.downloadLink);
    SetChecked(IDC_MENU_FIREFOX_ALL, menus_.firefox.downloadAll);
    return true;
}

bool BrowserMenuDialog::OnCommand(int id, int code, HWND control)
{
    if (id != IDOK) {
        return Dialog::OnCommand(id, code, control);
    }
    BrowserMenus menus;
    menus.chromium.downloadLink = Checked(IDC_MENU_CHROMIUM_LINK);
    menus.chromium.downloadAll = Checked(IDC_MENU_CHROMIUM_ALL);
    menus.firefox.downloadLink = Checked(IDC_MENU_FIREFOX_LINK);
    menus.firefox.downloadAll = Checked(IDC_MENU_FIREFOX_ALL);
    result_ = menus;
    Close(IDOK);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Media panel

MediaPanelDialog::MediaPanelDialog(const Theme& theme, MediaPanelSettings settings)
    : Dialog(IDD_MEDIA_PANEL, theme), settings_(std::move(settings))
{
}

std::optional<MediaPanelSettings> MediaPanelDialog::Run(HWND owner)
{
    RunModal(owner);
    return result_;
}

bool MediaPanelDialog::OnInit()
{
    SetChecked(IDC_MEDIA_ENABLED, settings_.enabled);
    SetText(IDC_MEDIA_TYPES, base::ToUtf16(settings_.types));
    SetNumber(IDC_MEDIA_MINIMUM, settings_.minimumKilobytes);
    SetText(IDC_MEDIA_SITES, base::ToUtf16(settings_.siteExceptions));
    SetChecked(IDC_MEDIA_DRM, settings_.drmNotice);
    return true;
}

bool MediaPanelDialog::OnCommand(int id, int code, HWND control)
{
    if (id == IDC_MEDIA_TYPES_DEFAULT) {
        SetText(IDC_MEDIA_TYPES, base::ToUtf16(model::DefaultMediaTypes()));
        return true;
    }
    if (id != IDOK) {
        return Dialog::OnCommand(id, code, control);
    }
    const auto minimum = Number(IDC_MEDIA_MINIMUM);
    if (!minimum || *minimum < 0 || *minimum > kMaxMinimumKilobytes) {
        Warn(L"Enter the smallest file size to offer in KB, from 0 to " + std::to_wstring(kMaxMinimumKilobytes) + L".");
        return true;
    }
    MediaPanelSettings settings;
    settings.enabled = Checked(IDC_MEDIA_ENABLED);
    settings.types = Words(Text(IDC_MEDIA_TYPES));
    settings.minimumKilobytes = *minimum;
    settings.siteExceptions = Words(Text(IDC_MEDIA_SITES));
    settings.drmNotice = Checked(IDC_MEDIA_DRM);
    result_ = std::move(settings);
    Close(IDOK);
    return true;
}

}  // namespace ldm::ui
