#include "ui/dialogs/OptionsDialog.h"

#include "base/Utf.h"
#include "resource.h"
#include "ui/FileDialogs.h"
#include "ui/dialogs/CaptureDialogs.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <filesystem>
#include <sstream>
#include <vector>

namespace ldm::ui {

namespace {

struct PageInfo {
    int templateId;
    const wchar_t* title;
};

// The Options dialog whose page keys the hook handles. Hook procedures get no context pointer;
// one modal Options dialog exists at a time, on the UI thread.
thread_local OptionsDialog* pageKeyTarget = nullptr;

constexpr std::array<PageInfo, 8> kPages{{
    {IDD_OPTIONS_GENERAL, L"General"},
    {IDD_OPTIONS_FILETYPES, L"File types"},
    {IDD_OPTIONS_SAVE, L"Save to"},
    {IDD_OPTIONS_DOWNLOADS, L"Downloads"},
    {IDD_OPTIONS_CONNECTION, L"Connection"},
    {IDD_OPTIONS_PROXY, L"Proxy / Socks"},
    {IDD_OPTIONS_LOGINS, L"Site logins"},
    {IDD_OPTIONS_SOUNDS, L"Sounds"},
}};

constexpr std::array<std::pair<model::DuplicatePolicy, const wchar_t*>, 4> kDuplicateChoices{{
    {model::DuplicatePolicy::Ask, L"Ask me"},
    {model::DuplicatePolicy::AddNumbered, L"Add it with a numbered name"},
    {model::DuplicatePolicy::Overwrite, L"Replace the existing download"},
    {model::DuplicatePolicy::ResumeOrShowComplete, L"Resume it or show it if complete"},
}};

constexpr std::array<std::pair<model::ProgressStartView, const wchar_t*>, 3> kViewChoices{{
    {model::ProgressStartView::Normal, L"Show normally"},
    {model::ProgressStartView::Minimized, L"Show minimized"},
    {model::ProgressStartView::Hidden, L"Do not show"},
}};

constexpr std::array<std::pair<model::SpeedProfile, const wchar_t*>, 3> kProfiles{{
    {model::SpeedProfile::Low, L"Low speed (mobile, satellite)"},
    {model::SpeedProfile::Medium, L"Medium speed (DSL)"},
    {model::SpeedProfile::High, L"High speed (cable, fiber, LAN)"},
}};

constexpr int kFileTypesPage = 1;
constexpr int kSavePage = 2;

// A file type as Options > File types accepts it: letters, digits and a few symbols, with '*' as a
// wildcard and an optional leading dot (the extension ignores anything else).
bool IsFileTypePattern(const std::wstring& word)
{
    const size_t start = !word.empty() && word.front() == L'.' ? 1 : 0;
    return word.size() > start && word.size() - start <= 32 &&
           std::all_of(word.begin() + static_cast<std::ptrdiff_t>(start), word.end(), [](wchar_t c) {
               return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'*' ||
                      c == L'?' || c == L'_' || c == L'+' || c == L'~' || c == L'-';
           });
}

// Space-separated words of a multi-line edit, joined by single spaces.
std::vector<std::wstring> SplitWords(const std::wstring& text)
{
    std::vector<std::wstring> words;
    std::wistringstream stream(text);
    for (std::wstring word; stream >> word;) {
        words.push_back(std::move(word));
    }
    return words;
}

std::wstring JoinWords(const std::vector<std::wstring>& words, const wchar_t* separator)
{
    std::wstring text;
    for (const std::wstring& word : words) {
        if (!text.empty()) {
            text += separator;
        }
        text += word;
    }
    return text;
}

int ConnectionsForProfile(model::SpeedProfile profile)
{
    switch (profile) {
    case model::SpeedProfile::Low:
        return 2;
    case model::SpeedProfile::Medium:
        return 4;
    case model::SpeedProfile::High:
        return 8;
    }
    return 8;
}

void AddColumn(HWND list, int index, const wchar_t* title, int width96)
{
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = const_cast<wchar_t*>(title);  // copied by the list view
    column.cx = ::MulDiv(width96, static_cast<int>(::GetDpiForWindow(list)), 96);
    ListView_InsertColumn(list, index, &column);
}

void SetRow(HWND list, int row, const std::wstring& first, const std::wstring& second)
{
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.pszText = const_cast<wchar_t*>(first.c_str());  // copied by the list view
    ListView_InsertItem(list, &item);
    ListView_SetItemText(list, row, 1, const_cast<wchar_t*>(second.c_str()));
}

}  // namespace

OptionsDialog::OptionsDialog(const Theme& theme, Input input, core::SecretProtector& secrets, SystemActions& system)
    : Dialog(IDD_OPTIONS, theme), input_(std::move(input)), secrets_(secrets), system_(system)
{
    categories_ = input_.categories;
    limits_ = input_.serverLimits;
    for (const core::SiteLoginView& login : input_.siteLogins) {
        logins_.push_back(core::SiteLoginInput{login.pattern, login.username, {}});
    }
}

std::optional<OptionsDialog::Output> OptionsDialog::Run(HWND owner)
{
    RunModal(owner);
    return result_;
}

INT_PTR CALLBACK OptionsDialog::PageProc(HWND page, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_INITDIALOG) {
        ::SetWindowLongPtrW(page, DWLP_USER, lParam);
        return TRUE;
    }
    auto* self = reinterpret_cast<OptionsDialog*>(::GetWindowLongPtrW(page, DWLP_USER));  // set at WM_INITDIALOG
    if (self == nullptr) {
        return FALSE;
    }
    switch (message) {
    case WM_COMMAND:
        self->OnPageCommand(LOWORD(wParam), HIWORD(wParam));
        return TRUE;
    case WM_NOTIFY:
        self->OnPageNotify(*reinterpret_cast<const NMHDR*>(lParam));
        return FALSE;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        if (self->CurrentTheme().IsDark()) {
            const bool editable = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX;
            return reinterpret_cast<INT_PTR>(self->CurrentTheme().OnCtlColor(reinterpret_cast<HDC>(wParam), editable));
        }
        return FALSE;
    default:
        return FALSE;
    }
}

bool OptionsDialog::OnInit()
{
    const HWND tab = Item(IDC_OPT_TAB);
    for (int i = 0; i < kPageCount; ++i) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(kPages[static_cast<size_t>(i)].title);  // copied by the tab control
        TabCtrl_InsertItem(tab, i, &item);
    }
    // The page area excludes the tab strip, so it is measured once the tabs exist.
    RECT area{};
    ::GetWindowRect(tab, &area);
    ::MapWindowPoints(nullptr, Handle(), reinterpret_cast<POINT*>(&area), 2);
    TabCtrl_AdjustRect(tab, FALSE, &area);
    for (int i = 0; i < kPageCount; ++i) {
        const HWND page = ::CreateDialogParamW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kPages[static_cast<size_t>(i)].templateId),
                                               Handle(), &OptionsDialog::PageProc, reinterpret_cast<LPARAM>(this));
        pages_[static_cast<size_t>(i)] = page;
        if (!CurrentTheme().IsDark()) {
            ::EnableThemeDialogTexture(page, ETDT_ENABLETAB);
        }
        CurrentTheme().ApplyToChildren(page);
        ::SetWindowPos(page, tab, area.left, area.top, area.right - area.left, area.bottom - area.top, SWP_NOACTIVATE);
    }
    const HWND limits = Control(IDC_OPT_LIMITS);
    const HWND logins = Control(IDC_OPT_LOGINS);
    const HWND browsers = Control(IDC_OPT_BROWSERS);
    for (const HWND list : {limits, logins, browsers}) {
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        if (CurrentTheme().IsDark()) {
            ListView_SetBkColor(list, CurrentTheme().Colors().window);
            ListView_SetTextBkColor(list, CurrentTheme().Colors().window);
            ListView_SetTextColor(list, CurrentTheme().Colors().text);
        }
    }
    AddColumn(limits, 0, L"Server", 200);
    AddColumn(limits, 1, L"Connections", 80);
    AddColumn(logins, 0, L"Server", 300);
    AddColumn(logins, 1, L"Login", 140);
    AddColumn(browsers, 0, L"Browser", 170);
    AddColumn(browsers, 1, L"Extension", 90);
    AddColumn(browsers, 2, L"Private windows", 130);
    ShowConnectedBrowsers();
    ::SendMessageW(Control(IDC_OPT_CONNECTIONS_SPIN), UDM_SETRANGE32, 1, 32);
    for (const auto& [policy, text] : kDuplicateChoices) {
        ComboBox_AddString(Control(IDC_OPT_DUPLICATE), text);
    }
    for (const auto& [view, text] : kViewChoices) {
        ComboBox_AddString(Control(IDC_OPT_PROGRESS_VIEW), text);
    }
    for (const auto& [profile, text] : kProfiles) {
        ComboBox_AddString(Control(IDC_OPT_PROFILE), text);
    }
    for (const model::Category& category : categories_) {
        ComboBox_AddString(Control(IDC_OPT_CATEGORY), base::ToUtf16(category.name).c_str());
    }
    Load();
    ShowPage(0);
    pageKeyTarget = this;
    pageKeyHook_ = ::SetWindowsHookExW(WH_MSGFILTER, &OptionsDialog::PageKeyFilter, nullptr, ::GetCurrentThreadId());
    return true;
}

void OptionsDialog::OnDestroying()
{
    if (pageKeyHook_ != nullptr) {
        ::UnhookWindowsHookEx(pageKeyHook_);
        pageKeyHook_ = nullptr;
    }
    if (pageKeyTarget == this) {
        pageKeyTarget = nullptr;
    }
}

LRESULT CALLBACK OptionsDialog::PageKeyFilter(int code, WPARAM wParam, LPARAM lParam)
{
    const auto* message = reinterpret_cast<const MSG*>(lParam);  // documented for WH_MSGFILTER
    OptionsDialog* dialog = pageKeyTarget;
    if (code == MSGF_DIALOGBOX && dialog != nullptr && message != nullptr && message->message == WM_KEYDOWN &&
        ::GetKeyState(VK_CONTROL) < 0 &&
        (message->hwnd == dialog->Handle() || ::IsChild(dialog->Handle(), message->hwnd))) {
        int delta = 0;
        if (message->wParam == VK_TAB) {
            delta = ::GetKeyState(VK_SHIFT) < 0 ? -1 : 1;
        } else if (message->wParam == VK_NEXT) {
            delta = 1;
        } else if (message->wParam == VK_PRIOR) {
            delta = -1;
        }
        if (delta != 0) {
            dialog->StepPage(delta);
            return 1;  // consumed
        }
    }
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

void OptionsDialog::StepPage(int delta)
{
    const HWND tab = Item(IDC_OPT_TAB);
    const int next = ((TabCtrl_GetCurSel(tab) + delta) % kPageCount + kPageCount) % kPageCount;
    TabCtrl_SetCurSel(tab, next);
    ShowPage(next);
}

void OptionsDialog::ShowPage(int index)
{
    for (int i = 0; i < kPageCount; ++i) {
        ::ShowWindow(pages_[static_cast<size_t>(i)], i == index ? SW_SHOW : SW_HIDE);
    }
}

HWND OptionsDialog::Control(int id) const
{
    for (const HWND page : pages_) {
        if (const HWND control = ::GetDlgItem(page, id); control != nullptr) {
            return control;
        }
    }
    return nullptr;
}

std::wstring OptionsDialog::Get(int id) const
{
    const HWND control = Control(id);
    const int length = ::GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    text.resize(static_cast<size_t>(::GetWindowTextW(control, text.data(), length + 1)));
    return text;
}

void OptionsDialog::Put(int id, const std::wstring& text)
{
    ::SetWindowTextW(Control(id), text.c_str());
}

bool OptionsDialog::IsChecked(int id) const
{
    return Button_GetCheck(Control(id)) == BST_CHECKED;
}

void OptionsDialog::Check(int id, bool checked)
{
    Button_SetCheck(Control(id), checked ? BST_CHECKED : BST_UNCHECKED);
}

std::optional<int> OptionsDialog::GetNumber(int id) const
{
    const std::wstring text = Get(id);
    if (text.empty()) {
        return std::nullopt;
    }
    wchar_t* end = nullptr;
    const long value = std::wcstol(text.c_str(), &end, 10);
    if (end == nullptr || *end != L'\0') {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

void OptionsDialog::Load()
{
    const model::Settings& s = input_.settings;
    Check(IDC_OPT_STARTUP, input_.launchOnStartup);
    Check(IDC_OPT_CLIPBOARD, s.monitorClipboard);

    captureKeys_ = s.captureKeys;
    browserMenus_ = BrowserMenus{s.chromiumMenu, s.firefoxMenu};
    Put(IDC_OPT_TYPES, base::ToUtf16(s.capturedExtensions));
    Put(IDC_OPT_SITES, base::ToUtf16(s.captureSiteExceptions));
    Check(IDC_OPT_OFFER_EXCEPTIONS, s.offerCaptureExceptions);
    addressExceptions_ = SplitWords(base::ToUtf16(s.captureAddressExceptions));
    ShowAddressExceptionCount();

    if (!categories_.empty()) {
        ComboBox_SetCurSel(Control(IDC_OPT_CATEGORY), 0);
        LoadCategory(0);
    }
    Put(IDC_OPT_TEMP, base::ToUtf16(s.temporaryDirectory));
    Check(IDC_OPT_FILE_DATE, s.setFileDateFromServer);

    Check(IDC_OPT_START_DIALOG, s.showStartDialog);
    Check(IDC_OPT_SPECULATIVE, s.speculativeStart);
    Check(IDC_OPT_QUEUE_ONLY, s.queueOnly);
    Check(IDC_OPT_COMPLETE_DIALOG, s.showCompleteDialog);
    Check(IDC_OPT_IGNORE_TIME, s.ignoreModificationTimeChanges);
    for (size_t i = 0; i < kDuplicateChoices.size(); ++i) {
        if (kDuplicateChoices[i].first == s.duplicatePolicy) {
            ComboBox_SetCurSel(Control(IDC_OPT_DUPLICATE), static_cast<int>(i));
        }
    }
    for (size_t i = 0; i < kViewChoices.size(); ++i) {
        if (kViewChoices[i].first == s.progressStartView) {
            ComboBox_SetCurSel(Control(IDC_OPT_PROGRESS_VIEW), static_cast<int>(i));
        }
    }
    Check(IDC_OPT_SPEED_TAB, s.progressShowSpeedTab);
    Check(IDC_OPT_DONE_TAB, s.progressShowCompletionTab);
    Put(IDC_OPT_USER_AGENT, base::ToUtf16(s.userAgent));
    Put(IDC_OPT_SCANNER, base::ToUtf16(s.virusScan.program));
    Put(IDC_OPT_SCANNER_ARGS, base::ToUtf16(s.virusScan.arguments));
    UpdateStartDialogControls();

    for (size_t i = 0; i < kProfiles.size(); ++i) {
        if (kProfiles[i].first == s.speedProfile) {
            ComboBox_SetCurSel(Control(IDC_OPT_PROFILE), static_cast<int>(i));
        }
    }
    Put(IDC_OPT_CONNECTIONS, std::to_wstring(s.defaultMaxConnections));
    RefreshLimits();
    Check(IDC_OPT_QUOTA_ON, s.quota.enabled);
    Put(IDC_OPT_QUOTA_MB, std::to_wstring(s.quota.megabytes));
    Put(IDC_OPT_QUOTA_HOURS, std::to_wstring(s.quota.hours));
    Check(IDC_OPT_QUOTA_WARN, s.quota.warnBeforeStopping);
    UpdateQuotaControls();

    Check(IDC_OPT_PROXY_NONE, s.proxyMode == model::ProxyMode::None);
    Check(IDC_OPT_PROXY_SYSTEM, s.proxyMode == model::ProxyMode::System);
    Check(IDC_OPT_PROXY_PAC, s.proxyMode == model::ProxyMode::AutoConfigScript);
    Check(IDC_OPT_PROXY_MANUAL, s.proxyMode == model::ProxyMode::Manual);
    Put(IDC_OPT_PAC_URL, base::ToUtf16(s.autoConfigScriptUrl));
    LoadProxy(IDC_OPT_HTTP_ON, IDC_OPT_HTTP_HOST, IDC_OPT_HTTP_PORT, IDC_OPT_HTTP_USER, s.httpProxy);
    LoadProxy(IDC_OPT_HTTPS_ON, IDC_OPT_HTTPS_HOST, IDC_OPT_HTTPS_PORT, IDC_OPT_HTTPS_USER, s.httpsProxy);
    LoadProxy(IDC_OPT_FTP_ON, IDC_OPT_FTP_HOST, IDC_OPT_FTP_PORT, IDC_OPT_FTP_USER, s.ftpProxy);
    Check(IDC_OPT_SOCKS_ON, s.socks.enabled);
    Put(IDC_OPT_SOCKS_HOST, base::ToUtf16(s.socks.host));
    Put(IDC_OPT_SOCKS_PORT, s.socks.port > 0 ? std::to_wstring(s.socks.port) : L"");
    Put(IDC_OPT_SOCKS_USER, base::ToUtf16(s.socks.user));
    Check(IDC_OPT_SOCKS4, s.socks.version == 4);
    Check(IDC_OPT_SOCKS5, s.socks.version != 4);
    Check(IDC_OPT_SOCKS_DNS, s.socks.remoteDns);
    Put(IDC_OPT_BYPASS, base::ToUtf16(s.proxyBypass));
    Check(IDC_OPT_FTP_PASSIVE, s.ftpPassive);
    for (const int id : {IDC_OPT_HTTP_PASS, IDC_OPT_HTTPS_PASS, IDC_OPT_FTP_PASS, IDC_OPT_SOCKS_PASS}) {
        Edit_SetCueBannerText(Control(id), L"(unchanged)");
    }
    UpdateProxyControls();

    RefreshLogins();
    Edit_SetCueBannerText(Control(IDC_OPT_LOGIN_PASSWORD), L"(unchanged when updating)");

    Check(IDC_OPT_SOUND_DONE_ON, s.soundComplete.enabled);
    Put(IDC_OPT_SOUND_DONE_FILE, base::ToUtf16(s.soundComplete.file));
    Check(IDC_OPT_SOUND_FAIL_ON, s.soundFailed.enabled);
    Put(IDC_OPT_SOUND_FAIL_FILE, base::ToUtf16(s.soundFailed.file));
}

void OptionsDialog::LoadCategory(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= categories_.size()) {
        return;
    }
    currentCategory_ = index;
    const model::Category& category = categories_[static_cast<size_t>(index)];
    Put(IDC_OPT_CAT_TYPES, base::ToUtf16(category.extensions));
    Put(IDC_OPT_CAT_FOLDER, base::ToUtf16(category.folder));
    Check(IDC_OPT_CAT_REMEMBER, category.rememberLastFolder);
}

void OptionsDialog::StoreCategory(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= categories_.size()) {
        return;
    }
    model::Category& category = categories_[static_cast<size_t>(index)];
    std::string extensions = base::ToUtf8(Get(IDC_OPT_CAT_TYPES));
    std::replace_if(extensions.begin(), extensions.end(), [](char c) { return c == '\r' || c == '\n' || c == ','; }, ' ');
    category.extensions = extensions;
    category.folder = base::ToUtf8(Get(IDC_OPT_CAT_FOLDER));
    category.rememberLastFolder = IsChecked(IDC_OPT_CAT_REMEMBER);
}

void OptionsDialog::LoadProxy(int enabledId, int hostId, int portId, int userId, const model::ProxyServer& server)
{
    Check(enabledId, server.enabled);
    Put(hostId, base::ToUtf16(server.host));
    Put(portId, server.port > 0 ? std::to_wstring(server.port) : L"");
    Put(userId, base::ToUtf16(server.user));
}

bool OptionsDialog::StoreProxy(int enabledId, int hostId, int portId, int userId, int passwordId, model::ProxyServer& server)
{
    server.enabled = IsChecked(enabledId);
    server.host = base::ToUtf8(Get(hostId));
    server.port = GetNumber(portId).value_or(0);
    server.user = base::ToUtf8(Get(userId));
    if (server.enabled && (server.host.empty() || server.port < 1 || server.port > 65535)) {
        Warn(L"Each enabled proxy needs a server and a port between 1 and 65535.");
        return false;
    }
    const std::string password = base::ToUtf8(Get(passwordId));
    if (server.user.empty()) {
        server.secret.clear();
    } else if (!password.empty()) {
        auto secret = secrets_.Protect(password);
        if (!secret) {
            Warn(L"The proxy password could not be protected.");
            return false;
        }
        server.secret = std::move(secret).Value();
    }
    return true;
}

void OptionsDialog::ShowConnectedBrowsers()
{
    const HWND list = Control(IDC_OPT_BROWSERS);
    const auto& browsers = input_.connectedBrowsers;
    for (size_t i = 0; i < browsers.size(); ++i) {
        const browser::Hello& hello = browsers[i];
        const std::wstring name = base::ToUtf16(hello.browser) +
                                  (hello.browserVersion.empty() ? L"" : L" " + base::ToUtf16(hello.browserVersion));
        const int row = static_cast<int>(i);
        SetRow(list, row, name, base::ToUtf16(hello.extensionVersion));
        std::wstring incognito = hello.incognitoAllowed ? L"Allowed" : L"Not allowed";
        ListView_SetItemText(list, row, 2, incognito.data());
    }
    const bool privateBlocked = std::any_of(browsers.begin(), browsers.end(),
                                            [](const browser::Hello& hello) { return !hello.incognitoAllowed; });
    Put(IDC_OPT_BROWSERS_HINT,
        browsers.empty()
            ? L"No browser is connected. Add the Last Download Manager extension to Chrome, Edge or Firefox; it "
              L"connects by itself while LDM runs."
        : privateBlocked
            ? L"For private windows, open the browser's extensions page, choose Details for Last Download Manager and "
              L"allow it in Incognito (Chrome), InPrivate (Edge) or Private Windows (Firefox)."
            : L"Click the LDM button in a browser's toolbar to turn capturing on or off for that browser.");
}

void OptionsDialog::RefreshLimits()
{
    const HWND list = Control(IDC_OPT_LIMITS);
    ListView_DeleteAllItems(list);
    for (size_t i = 0; i < limits_.size(); ++i) {
        SetRow(list, static_cast<int>(i), base::ToUtf16(limits_[i].pattern), std::to_wstring(limits_[i].maxConnections));
    }
}

void OptionsDialog::RefreshLogins()
{
    const HWND list = Control(IDC_OPT_LOGINS);
    ListView_DeleteAllItems(list);
    for (size_t i = 0; i < logins_.size(); ++i) {
        SetRow(list, static_cast<int>(i), base::ToUtf16(logins_[i].pattern), base::ToUtf16(logins_[i].username));
    }
}

void OptionsDialog::UpdateProxyControls()
{
    ::EnableWindow(Control(IDC_OPT_PAC_URL), IsChecked(IDC_OPT_PROXY_PAC));
    const bool manual = IsChecked(IDC_OPT_PROXY_MANUAL);
    for (int id = IDC_OPT_HTTP_ON; id <= IDC_OPT_SOCKS_DNS; ++id) {
        ::EnableWindow(Control(id), manual);
    }
    ::EnableWindow(Control(IDC_OPT_BYPASS), manual);
}

void OptionsDialog::UpdateStartDialogControls()
{
    const bool dialog = IsChecked(IDC_OPT_START_DIALOG);
    ::EnableWindow(Control(IDC_OPT_SPECULATIVE), dialog);
    ::EnableWindow(Control(IDC_OPT_QUEUE_ONLY), !dialog);
}

void OptionsDialog::UpdateQuotaControls()
{
    const bool enabled = IsChecked(IDC_OPT_QUOTA_ON);
    for (const int id : {IDC_OPT_QUOTA_MB, IDC_OPT_QUOTA_HOURS, IDC_OPT_QUOTA_WARN}) {
        ::EnableWindow(Control(id), enabled);
    }
}

void OptionsDialog::ShowAddressExceptionCount()
{
    const size_t count = addressExceptions_.size();
    Put(IDC_OPT_ADDRESSES_COUNT, count == 0   ? L"(none)"
                                 : count == 1 ? L"(1 address)"
                                              : L"(" + std::to_wstring(count) + L" addresses)");
}

void OptionsDialog::OnPageCommand(int id, int code)
{
    switch (id) {
    case IDC_OPT_KEYS: {
        CaptureKeysDialog dialog(CurrentTheme(), captureKeys_);
        if (auto keys = dialog.Run(Handle())) {
            captureKeys_ = *keys;
        }
        break;
    }
    case IDC_OPT_MENU: {
        BrowserMenuDialog dialog(CurrentTheme(), browserMenus_);
        if (auto menus = dialog.Run(Handle())) {
            browserMenus_ = *menus;
        }
        break;
    }
    case IDC_OPT_TYPES_DEFAULT:
        Put(IDC_OPT_TYPES, base::ToUtf16(model::DefaultCapturedExtensions()));
        break;
    case IDC_OPT_SITES_DEFAULT:
        Put(IDC_OPT_SITES, base::ToUtf16(model::DefaultCaptureSiteExceptions()));
        break;
    case IDC_OPT_ADDRESSES: {
        AddressExceptionsDialog dialog(CurrentTheme(), addressExceptions_);
        if (auto edited = dialog.Run(Handle())) {
            addressExceptions_ = std::move(*edited);
            ShowAddressExceptionCount();
        }
        break;
    }
    case IDC_OPT_CATEGORY:
        if (code == CBN_SELCHANGE) {
            StoreCategory(currentCategory_);
            LoadCategory(ComboBox_GetCurSel(Control(IDC_OPT_CATEGORY)));
        }
        break;
    case IDC_OPT_CAT_BROWSE:
        if (auto folder = PickFolder(Handle(), Get(IDC_OPT_CAT_FOLDER))) {
            Put(IDC_OPT_CAT_FOLDER, *folder);
        }
        break;
    case IDC_OPT_TEMP_BROWSE:
        if (auto folder = PickFolder(Handle(), Get(IDC_OPT_TEMP))) {
            Put(IDC_OPT_TEMP, *folder);
        }
        break;
    case IDC_OPT_START_DIALOG:
        UpdateStartDialogControls();
        break;
    case IDC_OPT_QUOTA_ON:
        UpdateQuotaControls();
        break;
    case IDC_OPT_SCANNER_BROWSE:
        if (auto program = PickOpenFile(Handle(), L"Programs", L"*.exe")) {
            Put(IDC_OPT_SCANNER, *program);
        }
        break;
    case IDC_OPT_PROFILE:
        if (code == CBN_SELCHANGE) {
            const int index = ComboBox_GetCurSel(Control(IDC_OPT_PROFILE));
            if (index >= 0) {
                Put(IDC_OPT_CONNECTIONS, std::to_wstring(ConnectionsForProfile(kProfiles[static_cast<size_t>(index)].first)));
            }
        }
        break;
    case IDC_OPT_LIMIT_ADD: {
        const std::string server = base::ToUtf8(Get(IDC_OPT_LIMIT_SERVER));
        const auto count = GetNumber(IDC_OPT_LIMIT_COUNT);
        if (server.empty() || !count || *count < 1 || *count > 32) {
            Warn(L"Enter a server, such as *.example.com, and between 1 and 32 connections.");
            break;
        }
        const auto existing = std::find_if(limits_.begin(), limits_.end(), [&](const auto& l) { return l.pattern == server; });
        if (existing != limits_.end()) {
            existing->maxConnections = *count;
        } else {
            limits_.push_back(model::ServerLimit{server, *count});
        }
        limitsEdited_ = true;
        RefreshLimits();
        break;
    }
    case IDC_OPT_LIMIT_REMOVE: {
        const int selected = ListView_GetNextItem(Control(IDC_OPT_LIMITS), -1, LVNI_SELECTED);
        if (selected >= 0 && static_cast<size_t>(selected) < limits_.size()) {
            limits_.erase(limits_.begin() + selected);
            limitsEdited_ = true;
            RefreshLimits();
        }
        break;
    }
    case IDC_OPT_PROXY_NONE:
    case IDC_OPT_PROXY_SYSTEM:
    case IDC_OPT_PROXY_PAC:
    case IDC_OPT_PROXY_MANUAL:
        UpdateProxyControls();
        break;
    case IDC_OPT_LOGIN_ADD: {
        core::SiteLoginInput login{base::ToUtf8(Get(IDC_OPT_LOGIN_PATTERN)), base::ToUtf8(Get(IDC_OPT_LOGIN_USER)),
                                   base::ToUtf8(Get(IDC_OPT_LOGIN_PASSWORD))};
        if (login.pattern.empty() || login.username.empty()) {
            Warn(L"Enter a server and a login.");
            break;
        }
        const auto existing = std::find_if(logins_.begin(), logins_.end(), [&](const auto& l) { return l.pattern == login.pattern; });
        if (existing != logins_.end()) {
            if (login.password.empty() && existing->username == login.username) {
                login.password = existing->password;  // keep: empty means unchanged
            }
            *existing = login;
        } else {
            if (login.password.empty()) {
                Warn(L"Enter the password for the new login.");
                break;
            }
            logins_.push_back(login);
        }
        loginsEdited_ = true;
        Put(IDC_OPT_LOGIN_PASSWORD, L"");
        RefreshLogins();
        break;
    }
    case IDC_OPT_LOGIN_REMOVE: {
        const int selected = ListView_GetNextItem(Control(IDC_OPT_LOGINS), -1, LVNI_SELECTED);
        if (selected >= 0 && static_cast<size_t>(selected) < logins_.size()) {
            logins_.erase(logins_.begin() + selected);
            loginsEdited_ = true;
            RefreshLogins();
        }
        break;
    }
    case IDC_OPT_SOUND_DONE_BROWSE:
    case IDC_OPT_SOUND_FAIL_BROWSE:
        if (auto file = PickOpenFile(Handle(), L"Sounds", L"*.wav")) {
            Put(id == IDC_OPT_SOUND_DONE_BROWSE ? IDC_OPT_SOUND_DONE_FILE : IDC_OPT_SOUND_FAIL_FILE, *file);
        }
        break;
    case IDC_OPT_SOUND_DONE_PLAY:
    case IDC_OPT_SOUND_FAIL_PLAY: {
        const std::wstring file = Get(id == IDC_OPT_SOUND_DONE_PLAY ? IDC_OPT_SOUND_DONE_FILE : IDC_OPT_SOUND_FAIL_FILE);
        if (!file.empty()) {
            system_.PlaySoundFile(std::filesystem::path(file));
        }
        break;
    }
    default:
        break;
    }
}

void OptionsDialog::OnPageNotify(const NMHDR& header)
{
    if (header.code != LVN_ITEMCHANGED) {
        return;
    }
    const auto& change = reinterpret_cast<const NMLISTVIEW&>(header);
    if ((change.uNewState & LVIS_SELECTED) == 0 || change.iItem < 0) {
        return;
    }
    const auto row = static_cast<size_t>(change.iItem);
    if (header.idFrom == IDC_OPT_LIMITS && row < limits_.size()) {
        Put(IDC_OPT_LIMIT_SERVER, base::ToUtf16(limits_[row].pattern));
        Put(IDC_OPT_LIMIT_COUNT, std::to_wstring(limits_[row].maxConnections));
    } else if (header.idFrom == IDC_OPT_LOGINS && row < logins_.size()) {
        Put(IDC_OPT_LOGIN_PATTERN, base::ToUtf16(logins_[row].pattern));
        Put(IDC_OPT_LOGIN_USER, base::ToUtf16(logins_[row].username));
        Put(IDC_OPT_LOGIN_PASSWORD, L"");
    }
}

bool OptionsDialog::Store()
{
    model::Settings s = input_.settings;
    s.monitorClipboard = IsChecked(IDC_OPT_CLIPBOARD);

    const std::vector<std::wstring> types = SplitWords(Get(IDC_OPT_TYPES));
    for (const std::wstring& type : types) {
        if (!IsFileTypePattern(type)) {
            Warn(L"\"" + type + L"\" is not a file type. Use letters and digits, such as ZIP, with * as a wildcard.");
            ShowPage(kFileTypesPage);
            TabCtrl_SetCurSel(Item(IDC_OPT_TAB), kFileTypesPage);
            return false;
        }
    }
    s.capturedExtensions = base::ToUtf8(JoinWords(types, L" "));
    s.captureSiteExceptions = base::ToUtf8(JoinWords(SplitWords(Get(IDC_OPT_SITES)), L" "));
    s.captureAddressExceptions = base::ToUtf8(JoinWords(addressExceptions_, L"\n"));
    s.offerCaptureExceptions = IsChecked(IDC_OPT_OFFER_EXCEPTIONS);
    s.captureKeys = captureKeys_;
    s.chromiumMenu = browserMenus_.chromium;
    s.firefoxMenu = browserMenus_.firefox;

    StoreCategory(currentCategory_);
    for (const model::Category& category : categories_) {
        if (!std::filesystem::path(base::ToUtf16(category.folder)).is_absolute()) {
            Warn(L"The folder of category \"" + base::ToUtf16(category.name) + L"\" must be a full path.");
            ShowPage(kSavePage);
            TabCtrl_SetCurSel(Item(IDC_OPT_TAB), kSavePage);
            return false;
        }
    }
    const std::wstring temporary = Get(IDC_OPT_TEMP);
    if (!temporary.empty() && !std::filesystem::path(temporary).is_absolute()) {
        Warn(L"The temporary folder must be a full path, or empty.");
        return false;
    }
    s.temporaryDirectory = base::ToUtf8(temporary);
    s.setFileDateFromServer = IsChecked(IDC_OPT_FILE_DATE);

    s.showStartDialog = IsChecked(IDC_OPT_START_DIALOG);
    s.speculativeStart = IsChecked(IDC_OPT_SPECULATIVE);
    s.queueOnly = IsChecked(IDC_OPT_QUEUE_ONLY);
    s.showCompleteDialog = IsChecked(IDC_OPT_COMPLETE_DIALOG);
    s.ignoreModificationTimeChanges = IsChecked(IDC_OPT_IGNORE_TIME);
    if (const int index = ComboBox_GetCurSel(Control(IDC_OPT_DUPLICATE)); index >= 0) {
        s.duplicatePolicy = kDuplicateChoices[static_cast<size_t>(index)].first;
    }
    if (const int index = ComboBox_GetCurSel(Control(IDC_OPT_PROGRESS_VIEW)); index >= 0) {
        s.progressStartView = kViewChoices[static_cast<size_t>(index)].first;
    }
    s.progressShowSpeedTab = IsChecked(IDC_OPT_SPEED_TAB);
    s.progressShowCompletionTab = IsChecked(IDC_OPT_DONE_TAB);
    const std::string userAgent = base::ToUtf8(Get(IDC_OPT_USER_AGENT));
    s.userAgent = userAgent.empty() ? model::DefaultUserAgent() : userAgent;
    s.virusScan.program = base::ToUtf8(Get(IDC_OPT_SCANNER));
    s.virusScan.arguments = base::ToUtf8(Get(IDC_OPT_SCANNER_ARGS));

    if (const int index = ComboBox_GetCurSel(Control(IDC_OPT_PROFILE)); index >= 0) {
        s.speedProfile = kProfiles[static_cast<size_t>(index)].first;
    }
    const auto connections = GetNumber(IDC_OPT_CONNECTIONS);
    if (!connections || *connections < 1 || *connections > 32) {
        Warn(L"Connections per download must be between 1 and 32.");
        return false;
    }
    s.defaultMaxConnections = *connections;

    s.quota.enabled = IsChecked(IDC_OPT_QUOTA_ON);
    s.quota.warnBeforeStopping = IsChecked(IDC_OPT_QUOTA_WARN);
    const auto quotaMegabytes = GetNumber(IDC_OPT_QUOTA_MB);
    const auto quotaHours = GetNumber(IDC_OPT_QUOTA_HOURS);
    if (s.quota.enabled && (!quotaMegabytes || *quotaMegabytes < 1 || *quotaMegabytes > 10'000'000)) {
        Warn(L"The download quota must be between 1 and 10,000,000 MB.");
        return false;
    }
    if (s.quota.enabled && (!quotaHours || *quotaHours < 1 || *quotaHours > 23)) {
        Warn(L"The quota period must be between 1 and 23 hours.");
        return false;
    }
    if (quotaMegabytes && *quotaMegabytes >= 1 && *quotaMegabytes <= 10'000'000) {
        s.quota.megabytes = *quotaMegabytes;
    }
    if (quotaHours && *quotaHours >= 1 && *quotaHours <= 23) {
        s.quota.hours = *quotaHours;
    }

    s.proxyMode = IsChecked(IDC_OPT_PROXY_NONE)     ? model::ProxyMode::None
                  : IsChecked(IDC_OPT_PROXY_PAC)    ? model::ProxyMode::AutoConfigScript
                  : IsChecked(IDC_OPT_PROXY_MANUAL) ? model::ProxyMode::Manual
                                                    : model::ProxyMode::System;
    s.autoConfigScriptUrl = base::ToUtf8(Get(IDC_OPT_PAC_URL));
    if (s.proxyMode == model::ProxyMode::AutoConfigScript && s.autoConfigScriptUrl.empty()) {
        Warn(L"Enter the address of the automatic configuration script.");
        return false;
    }
    if (!StoreProxy(IDC_OPT_HTTP_ON, IDC_OPT_HTTP_HOST, IDC_OPT_HTTP_PORT, IDC_OPT_HTTP_USER, IDC_OPT_HTTP_PASS, s.httpProxy) ||
        !StoreProxy(IDC_OPT_HTTPS_ON, IDC_OPT_HTTPS_HOST, IDC_OPT_HTTPS_PORT, IDC_OPT_HTTPS_USER, IDC_OPT_HTTPS_PASS, s.httpsProxy) ||
        !StoreProxy(IDC_OPT_FTP_ON, IDC_OPT_FTP_HOST, IDC_OPT_FTP_PORT, IDC_OPT_FTP_USER, IDC_OPT_FTP_PASS, s.ftpProxy)) {
        return false;
    }
    model::ProxyServer socks{s.socks.enabled, s.socks.host, s.socks.port, s.socks.user, s.socks.secret};
    if (!StoreProxy(IDC_OPT_SOCKS_ON, IDC_OPT_SOCKS_HOST, IDC_OPT_SOCKS_PORT, IDC_OPT_SOCKS_USER, IDC_OPT_SOCKS_PASS, socks)) {
        return false;
    }
    s.socks.enabled = socks.enabled;
    s.socks.host = socks.host;
    s.socks.port = socks.port > 0 ? socks.port : 1080;
    s.socks.user = socks.user;
    s.socks.secret = socks.secret;
    s.socks.version = IsChecked(IDC_OPT_SOCKS4) ? 4 : 5;
    s.socks.remoteDns = IsChecked(IDC_OPT_SOCKS_DNS);
    s.proxyBypass = base::ToUtf8(Get(IDC_OPT_BYPASS));
    s.ftpPassive = IsChecked(IDC_OPT_FTP_PASSIVE);

    s.soundComplete = model::SoundEvent{IsChecked(IDC_OPT_SOUND_DONE_ON), base::ToUtf8(Get(IDC_OPT_SOUND_DONE_FILE))};
    s.soundFailed = model::SoundEvent{IsChecked(IDC_OPT_SOUND_FAIL_ON), base::ToUtf8(Get(IDC_OPT_SOUND_FAIL_FILE))};

    Output output;
    output.settings = std::move(s);
    for (size_t i = 0; i < categories_.size(); ++i) {
        const model::Category& before = input_.categories[i];
        const model::Category& after = categories_[i];
        if (before.extensions != after.extensions || before.folder != after.folder ||
            before.rememberLastFolder != after.rememberLastFolder) {
            output.changedCategories.push_back(after);
        }
    }
    if (limitsEdited_) {
        output.serverLimits = limits_;
    }
    if (loginsEdited_) {
        output.siteLogins = logins_;
    }
    output.launchOnStartup = IsChecked(IDC_OPT_STARTUP);
    result_ = std::move(output);
    return true;
}

void OptionsDialog::ApplyEditedFields(const model::Settings& edited, model::Settings& current)
{
    current.monitorClipboard = edited.monitorClipboard;
    current.capturedExtensions = edited.capturedExtensions;
    current.captureSiteExceptions = edited.captureSiteExceptions;
    current.captureAddressExceptions = edited.captureAddressExceptions;
    current.offerCaptureExceptions = edited.offerCaptureExceptions;
    current.captureKeys = edited.captureKeys;
    current.chromiumMenu = edited.chromiumMenu;
    current.firefoxMenu = edited.firefoxMenu;
    current.temporaryDirectory = edited.temporaryDirectory;
    current.setFileDateFromServer = edited.setFileDateFromServer;
    current.showStartDialog = edited.showStartDialog;
    current.speculativeStart = edited.speculativeStart;
    current.queueOnly = edited.queueOnly;
    current.showCompleteDialog = edited.showCompleteDialog;
    current.ignoreModificationTimeChanges = edited.ignoreModificationTimeChanges;
    current.duplicatePolicy = edited.duplicatePolicy;
    current.progressStartView = edited.progressStartView;
    current.progressShowSpeedTab = edited.progressShowSpeedTab;
    current.progressShowCompletionTab = edited.progressShowCompletionTab;
    current.userAgent = edited.userAgent;
    current.virusScan = edited.virusScan;
    current.speedProfile = edited.speedProfile;
    current.defaultMaxConnections = edited.defaultMaxConnections;
    current.quota = edited.quota;
    current.proxyMode = edited.proxyMode;
    current.autoConfigScriptUrl = edited.autoConfigScriptUrl;
    current.httpProxy = edited.httpProxy;
    current.httpsProxy = edited.httpsProxy;
    current.ftpProxy = edited.ftpProxy;
    current.socks = edited.socks;
    current.proxyBypass = edited.proxyBypass;
    current.ftpPassive = edited.ftpPassive;
    current.soundComplete = edited.soundComplete;
    current.soundFailed = edited.soundFailed;
}

bool OptionsDialog::OnCommand(int id, int code, HWND control)
{
    if (id == IDOK) {
        if (Store()) {
            Close(IDOK);
        }
        return true;
    }
    return Dialog::OnCommand(id, code, control);
}

bool OptionsDialog::OnNotify(const NMHDR& header, LRESULT& result)
{
    if (header.idFrom == IDC_OPT_TAB && header.code == TCN_SELCHANGE) {
        ShowPage(TabCtrl_GetCurSel(Item(IDC_OPT_TAB)));
        result = 0;
        return true;
    }
    return false;
}

}  // namespace ldm::ui
