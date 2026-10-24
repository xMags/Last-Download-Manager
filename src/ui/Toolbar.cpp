#include "ui/Toolbar.h"

#include "resource.h"
#include "ui/Dpi.h"
#include "ui/GlyphIcons.h"

#include <commctrl.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>

namespace ldm::ui {

namespace {

struct ButtonInfo {
    int command;
    GradientIcon icon;
    const wchar_t* label;
};

// Icon font codepoints used to build the toolbar icons.
constexpr wchar_t kArrowDown = 0xE74B;
constexpr wchar_t kRoundedSquare = 0xE71A;
constexpr wchar_t kCheck = 0xE73E;
constexpr wchar_t kGear = 0xE713;

// LDM's buttons in their default order (gap G-1), as thin gradient outlines. Five download
// actions use embedded icons (src/resources/toolbar); other icons use local drawings. Scheduler and
// queue buttons join in M4, when those features exist; "Tell a Friend" is excluded by the checklist.
constexpr std::array<ButtonInfo, 7> kButtons{{
    {ID_TASKS_ADD_URL, {{}, RGB(0x4C, 0xB8, 0x6E), RGB(0x2F, 0x8C, 0xC4), IconShape::OutlinePlus}, L"Add URL"},
    {ID_FILE_DOWNLOAD_NOW, {{{{kArrowDown, 46}}}, RGB(0x34, 0xA8, 0xE0), RGB(0x3A, 0x6F, 0xD8), IconShape::Ring}, L"Resume"},
    {ID_FILE_STOP, {{{{kRoundedSquare, 34}}}, RGB(0xF5, 0xA6, 0x23), RGB(0xE0, 0x5A, 0x3A), IconShape::Ring}, L"Stop"},
    {ID_DOWNLOADS_STOP_ALL, {{}, RGB(0xF0, 0x6A, 0x5A), RGB(0xC2, 0x3F, 0x8E), IconShape::RaisedHand}, L"Stop All"},
    {ID_FILE_REMOVE, {{}, RGB(0xE8, 0x50, 0x60), RGB(0xA8, 0x40, 0xB8), IconShape::Trash}, L"Delete"},
    {ID_DOWNLOADS_DELETE_COMPLETED, {{{{kCheck, 40, 0, 9}}}, RGB(0xE8, 0x3E, 0x9A), RGB(0x8A, 0x4F, 0xD8), IconShape::EmptyTrash},
     L"Delete Completed"},
    {ID_DOWNLOADS_OPTIONS, {{{{kGear, 86}}}, RGB(0x3F, 0x7A, 0xE8), RGB(0x9B, 0x51, 0xE0)}, L"Options"},
}};

struct ResourceIcon {
    int command;
    int normal;
    int disabled;
};

constexpr std::array<ResourceIcon, 5> kResourceIcons{{
    {ID_FILE_DOWNLOAD_NOW, IDI_TOOLBAR_RESUME, IDI_TOOLBAR_RESUME_DISABLED},
    {ID_FILE_STOP, IDI_TOOLBAR_STOP, IDI_TOOLBAR_STOP_DISABLED},
    {ID_DOWNLOADS_STOP_ALL, IDI_TOOLBAR_STOP_ALL, IDI_TOOLBAR_STOP_ALL_DISABLED},
    {ID_FILE_REMOVE, IDI_TOOLBAR_DELETE, IDI_TOOLBAR_DELETE_DISABLED},
    {ID_DOWNLOADS_DELETE_COMPLETED, IDI_TOOLBAR_DELETE_COMPLETED, IDI_TOOLBAR_DELETE_COMPLETED_DISABLED},
}};

void ApplyResourceIcons(HIMAGELIST images, int size, bool disabled)
{
    for (const ResourceIcon& resource : kResourceIcons) {
        const auto button = std::find_if(
            kButtons.begin(), kButtons.end(),
            [&resource](const ButtonInfo& candidate) { return candidate.command == resource.command; });
        if (button == kButtons.end()) {
            throw std::logic_error("Toolbar icon has no matching command");
        }
        const int resourceId = disabled ? resource.disabled : resource.normal;
        UniqueIcon icon(static_cast<HICON>(::LoadImageW(
            ::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resourceId), IMAGE_ICON, size, size, 0)));
        if (!icon ||
            ::ImageList_ReplaceIcon(images, static_cast<int>(button - kButtons.begin()), icon.get()) == -1) {
            throw std::runtime_error("Could not load an embedded toolbar icon");
        }
    }
}

std::vector<int> DefaultOrder()
{
    std::vector<int> order;
    for (const ButtonInfo& button : kButtons) {
        order.push_back(button.command);
    }
    return order;
}

std::vector<int> ParseOrder(const std::string& layout)
{
    std::vector<int> order;
    size_t pos = 0;
    while (pos < layout.size()) {
        size_t end = layout.find(',', pos);
        if (end == std::string::npos) {
            end = layout.size();
        }
        int command = -1;
        const auto [ptr, error] = std::from_chars(layout.data() + pos, layout.data() + end, command);
        pos = end + 1;
        if (error != std::errc{} || ptr != layout.data() + end) {
            continue;
        }
        const bool known = command == 0 || std::any_of(kButtons.begin(), kButtons.end(),
                                                        [command](const ButtonInfo& b) { return b.command == command; });
        const bool repeated = command != 0 && std::find(order.begin(), order.end(), command) != order.end();
        if (known && !repeated) {
            order.push_back(command);
        }
    }
    return std::any_of(order.begin(), order.end(), [](int command) { return command != 0; }) ? order : DefaultOrder();
}

// Large buttons all share one width; longer labels end in "..." and show whole in the tooltip.
constexpr int kLargeButtonWidth96 = 60;

TBBUTTON MakeButton(int command, bool autosize)
{
    TBBUTTON button{};
    if (command == 0) {
        button.fsStyle = BTNS_SEP;
        return button;
    }
    const auto it = std::find_if(kButtons.begin(), kButtons.end(), [command](const ButtonInfo& b) { return b.command == command; });
    button.iBitmap = static_cast<int>(it - kButtons.begin());
    button.idCommand = command;
    button.fsState = TBSTATE_ENABLED;
    button.fsStyle = autosize ? BTNS_BUTTON | BTNS_AUTOSIZE : BTNS_BUTTON;
    button.iString = reinterpret_cast<INT_PTR>(it->label);  // TBBUTTON stores strings as INT_PTR
    return button;
}

}  // namespace

void Toolbar::Create(HWND parent, int controlId, bool largeButtons, const std::string& layout, const Theme& theme)
{
    window_ = ::CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
                                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS |
                                    TBSTYLE_TRANSPARENT | CCS_NODIVIDER | CCS_NORESIZE | CCS_NOPARENTALIGN | CCS_ADJUSTABLE,
                                0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),  // child id in the HMENU slot
                                ::GetModuleHandleW(nullptr), nullptr);
    ::SendMessageW(window_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    large_ = largeButtons;
    savedOrder_ = ParseOrder(layout);
    Refresh(largeButtons, theme);
    ApplyLayout(layout);
}

void Toolbar::ApplyImages(const Theme& theme)
{
    const UINT dpi = DpiOf(window_);
    const int size = Scale(large_ ? 32 : 16, dpi);
    std::vector<GradientIcon> icons;
    for (const ButtonInfo& button : kButtons) {
        icons.push_back(button.icon);
    }
    images_ = CreateGradientImageList(icons, size, false);
    disabledImages_ = CreateGradientImageList(icons, size, true);
    ApplyResourceIcons(images_.get(), size, false);
    ApplyResourceIcons(disabledImages_.get(), size, true);
    ::SendMessageW(window_, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(images_.get()));
    ::SendMessageW(window_, TB_SETDISABLEDIMAGELIST, 0, reinterpret_cast<LPARAM>(disabledImages_.get()));
    textColor_ = theme.Colors().text;
    background_ = theme.Colors().panel;
    hotColor_ = theme.Colors().hot;
    dark_ = theme.IsDark();
    if (const auto tooltip = reinterpret_cast<HWND>(::SendMessageW(window_, TB_GETTOOLTIPS, 0, 0)); tooltip != nullptr) {  // returns the HWND
        theme.ApplyToControl(tooltip);
    }
}

void Toolbar::Refresh(bool largeButtons, const Theme& theme)
{
    large_ = largeButtons;
    const UINT dpi = DpiOf(window_);
    // Large buttons show their label under the icon; small ones show only the icon, the label
    // becoming the tooltip.
    LONG_PTR style = ::GetWindowLongPtrW(window_, GWL_STYLE);
    style = large_ ? (style & ~static_cast<LONG_PTR>(TBSTYLE_LIST)) : (style | TBSTYLE_LIST);
    ::SetWindowLongPtrW(window_, GWL_STYLE, style);
    ::SendMessageW(window_, TB_SETEXTENDEDSTYLE, 0,
                   large_ ? TBSTYLE_EX_DOUBLEBUFFER : (TBSTYLE_EX_DOUBLEBUFFER | TBSTYLE_EX_MIXEDBUTTONS));
    ApplyImages(theme);
    const int padding = Scale(large_ ? 10 : 6, dpi);
    ::SendMessageW(window_, TB_SETPADDING, 0, MAKELPARAM(padding, Scale(large_ ? 6 : 6, dpi)));
    ::SendMessageW(window_, TB_SETMAXTEXTROWS, large_ ? 1 : 0, 0);
    // Large: one fixed width with ellipsis. Small: icon-only buttons sized to their image.
    const int fixedWidth = Scale(kLargeButtonWidth96, dpi);
    ::SendMessageW(window_, TB_SETBUTTONWIDTH, 0, large_ ? MAKELPARAM(fixedWidth, fixedWidth) : MAKELPARAM(0, Scale(400, dpi)));
    ::SendMessageW(window_, TB_SETDRAWTEXTFLAGS, DT_END_ELLIPSIS, large_ ? DT_END_ELLIPSIS : 0);
    const int count = static_cast<int>(::SendMessageW(window_, TB_BUTTONCOUNT, 0, 0));
    for (int i = 0; i < count; ++i) {
        TBBUTTONINFOW info{sizeof(info)};
        info.dwMask = TBIF_BYINDEX | TBIF_STYLE;
        ::SendMessageW(window_, TB_GETBUTTONINFOW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&info));
        if ((info.fsStyle & BTNS_SEP) != 0) {
            continue;
        }
        info.fsStyle = large_ ? static_cast<BYTE>(info.fsStyle & ~BTNS_AUTOSIZE) : static_cast<BYTE>(info.fsStyle | BTNS_AUTOSIZE);
        ::SendMessageW(window_, TB_SETBUTTONINFOW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&info));
    }
    ::SendMessageW(window_, TB_AUTOSIZE, 0, 0);
    ::InvalidateRect(window_, nullptr, TRUE);
}

int Toolbar::PreferredHeight() const
{
    SIZE size{};
    ::SendMessageW(window_, TB_GETMAXSIZE, 0, reinterpret_cast<LPARAM>(&size));
    const DWORD buttonSize = static_cast<DWORD>(::SendMessageW(window_, TB_GETBUTTONSIZE, 0, 0));
    return std::max<int>(size.cy, HIWORD(buttonSize)) + Scale(4, DpiOf(window_));
}

void Toolbar::ApplyLayout(const std::string& layout)
{
    while (::SendMessageW(window_, TB_BUTTONCOUNT, 0, 0) > 0) {
        ::SendMessageW(window_, TB_DELETEBUTTON, 0, 0);
    }
    std::vector<TBBUTTON> buttons;
    for (const int command : ParseOrder(layout)) {
        buttons.push_back(MakeButton(command, !large_));
    }
    ::SendMessageW(window_, TB_ADDBUTTONSW, buttons.size(), reinterpret_cast<LPARAM>(buttons.data()));
    ::SendMessageW(window_, TB_AUTOSIZE, 0, 0);
}

void Toolbar::SetEnabled(int commandId, bool enabled)
{
    ::SendMessageW(window_, TB_ENABLEBUTTON, static_cast<WPARAM>(commandId), MAKELPARAM(enabled ? TRUE : FALSE, 0));
}

void Toolbar::Customize()
{
    savedOrder_ = ParseOrder(Layout());
    ::SendMessageW(window_, TB_CUSTOMIZE, 0, 0);
}

std::string Toolbar::Layout() const
{
    std::string layout;
    const int count = static_cast<int>(::SendMessageW(window_, TB_BUTTONCOUNT, 0, 0));
    for (int i = 0; i < count; ++i) {
        TBBUTTON button{};
        ::SendMessageW(window_, TB_GETBUTTON, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&button));
        if (!layout.empty()) {
            layout += ',';
        }
        layout += std::to_string((button.fsStyle & BTNS_SEP) != 0 ? 0 : button.idCommand);
    }
    return layout;
}

bool Toolbar::HandleNotify(const NMHDR& header, LRESULT& result, bool& layoutChanged)
{
    if (header.hwndFrom != window_) {
        return false;
    }
    switch (header.code) {
    case TBN_QUERYINSERT:
    case TBN_QUERYDELETE:
        result = TRUE;
        return true;
    case TBN_INITCUSTOMIZE:
        result = TBNRF_HIDEHELP;
        return true;
    case TBN_GETBUTTONINFOW: {
        auto& info = const_cast<NMTOOLBARW&>(reinterpret_cast<const NMTOOLBARW&>(header));  // the toolbar expects us to fill it
        if (info.iItem < 0 || info.iItem >= static_cast<int>(kButtons.size())) {
            result = FALSE;
            return true;
        }
        info.tbButton = MakeButton(kButtons[static_cast<size_t>(info.iItem)].command, !large_);
        if (info.pszText != nullptr && info.cchText > 0) {
            wcsncpy_s(info.pszText, static_cast<size_t>(info.cchText), kButtons[static_cast<size_t>(info.iItem)].label,
                      _TRUNCATE);
        }
        result = TRUE;
        return true;
    }
    case TBN_RESET: {
        std::string layout;
        for (const int command : savedOrder_) {
            layout += (layout.empty() ? "" : ",") + std::to_string(command);
        }
        ApplyLayout(layout);
        result = TBNRF_ENDCUSTOMIZE;
        return true;
    }
    case TBN_GETINFOTIPW: {
        // The whole label, which a fixed-width button may have cut short.
        auto& tip = const_cast<NMTBGETINFOTIPW&>(reinterpret_cast<const NMTBGETINFOTIPW&>(header));  // documented in-out
        const auto it = std::find_if(kButtons.begin(), kButtons.end(), [&tip](const ButtonInfo& b) { return b.command == tip.iItem; });
        if (it != kButtons.end() && tip.pszText != nullptr && tip.cchTextMax > 0) {
            wcsncpy_s(tip.pszText, static_cast<size_t>(tip.cchTextMax), it->label, _TRUNCATE);
        }
        result = 0;
        return true;
    }
    case TBN_TOOLBARCHANGE:
        layoutChanged = true;
        result = 0;
        return true;
    case NM_CUSTOMDRAW: {
        auto& draw = const_cast<NMTBCUSTOMDRAW&>(reinterpret_cast<const NMTBCUSTOMDRAW&>(header));  // documented in-out
        if (draw.nmcd.dwDrawStage == CDDS_PREPAINT) {
            if (dark_) {
                RECT client{};
                ::GetClientRect(window_, &client);
                UniqueBrush background(::CreateSolidBrush(background_));
                ::FillRect(draw.nmcd.hdc, &client, background.get());
            }
            result = CDRF_NOTIFYITEMDRAW;
            return true;
        }
        if (draw.nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            draw.clrText = textColor_;
            draw.clrBtnFace = background_;
            result = CDRF_DODEFAULT;
            if (dark_) {
                draw.clrTextHighlight = textColor_;
                draw.clrHighlightHotTrack = hotColor_;
                result |= TBCDRF_USECDCOLORS | TBCDRF_HILITEHOTTRACK | TBCDRF_NOETCHEDEFFECT;
            }
            return true;
        }
        result = CDRF_DODEFAULT;
        return true;
    }
    default:
        return false;
    }
}

}  // namespace ldm::ui
