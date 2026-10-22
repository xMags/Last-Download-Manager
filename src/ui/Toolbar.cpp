#include "ui/Toolbar.h"

#include "resource.h"
#include "ui/Dpi.h"
#include "ui/GlyphIcons.h"

#include <commctrl.h>

#include <algorithm>
#include <array>
#include <charconv>

namespace ldm::ui {

namespace {

struct ButtonInfo {
    int command;
    Glyph glyph;
    const wchar_t* label;
};

// LDM's buttons in the reference's default order (gap G-1). Scheduler and queue buttons join in M4, when
// those features exist; "Tell a Friend" is excluded by the checklist.
constexpr std::array<ButtonInfo, 7> kButtons{{
    {ID_TASKS_ADD_URL, {0xE710, RGB(0x1E, 0x88, 0xE5)}, L"Add URL"},
    {ID_FILE_DOWNLOAD_NOW, {0xE768, RGB(0x2E, 0xA0, 0x43)}, L"Resume"},
    {ID_FILE_STOP, {0xE769, RGB(0xF0, 0x8C, 0x00)}, L"Stop"},
    {ID_DOWNLOADS_STOP_ALL, {0xE71A, RGB(0xE5, 0x39, 0x35)}, L"Stop All"},
    {ID_FILE_REMOVE, {0xE74D, RGB(0xD3, 0x2F, 0x2F)}, L"Delete"},
    {ID_DOWNLOADS_DELETE_COMPLETED, {0xE930, RGB(0x8E, 0x44, 0xAD)}, L"Delete Completed"},
    {ID_DOWNLOADS_OPTIONS, {0xE713, RGB(0x60, 0x7D, 0x8B)}, L"Options"},
}};

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

TBBUTTON MakeButton(int command)
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
    button.fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE;
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
    std::vector<Glyph> glyphs;
    for (const ButtonInfo& button : kButtons) {
        glyphs.push_back(button.glyph);
    }
    images_ = CreateGlyphImageList(glyphs, size);
    disabledImages_ = CreateGlyphImageList(glyphs, size, true);
    ::SendMessageW(window_, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(images_.get()));
    ::SendMessageW(window_, TB_SETDISABLEDIMAGELIST, 0, reinterpret_cast<LPARAM>(disabledImages_.get()));
    textColor_ = theme.Colors().text;
    background_ = theme.Colors().panel;
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
        buttons.push_back(MakeButton(command));
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
        info.tbButton = MakeButton(kButtons[static_cast<size_t>(info.iItem)].command);
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
    case TBN_TOOLBARCHANGE:
        layoutChanged = true;
        result = 0;
        return true;
    case NM_CUSTOMDRAW: {
        auto& draw = const_cast<NMTBCUSTOMDRAW&>(reinterpret_cast<const NMTBCUSTOMDRAW&>(header));  // documented in-out
        if (draw.nmcd.dwDrawStage == CDDS_PREPAINT) {
            result = CDRF_NOTIFYITEMDRAW;
            return true;
        }
        if (draw.nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            draw.clrText = textColor_;
            draw.clrBtnFace = background_;
            result = CDRF_DODEFAULT;
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
