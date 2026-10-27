#include "ui/DarkControls.h"

#include "ui/Dpi.h"
#include "ui/Handles.h"
#include "ui/Theme.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <type_traits>

namespace ldm::ui {

namespace {

constexpr UINT_PTR kSubclassId = 1;

const Theme& ThemeOf(DWORD_PTR data)
{
    return *reinterpret_cast<const Theme*>(data);  // set by AttachDarkPainting
}

bool ClassIs(const wchar_t* className, const wchar_t* expected)
{
    return ::CompareStringOrdinal(className, -1, expected, -1, TRUE) == CSTR_EQUAL;
}

std::wstring WindowText(HWND window)
{
    const int length = ::GetWindowTextLengthW(window);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    text.resize(static_cast<size_t>(::GetWindowTextW(window, text.data(), length + 1)));
    return text;
}

HGDIOBJ ControlFont(HWND control)
{
    const auto font = reinterpret_cast<HFONT>(::SendMessageW(control, WM_GETFONT, 0, 0));  // WM_GETFONT returns the HFONT
    return font != nullptr ? font : ::GetStockObject(DEFAULT_GUI_FONT);
}

// Keyboard cues: accelerator underlines and focus rectangles appear only after keyboard use.
struct Cues {
    bool hideAccelerators = false;
    bool hideFocus = false;
};

Cues QueryCues(HWND control)
{
    const auto state = static_cast<UINT>(::SendMessageW(control, WM_QUERYUISTATE, 0, 0));
    return Cues{(state & UISF_HIDEACCEL) != 0, (state & UISF_HIDEFOCUS) != 0};
}

// The brush the parent dialog paints this control's background with.
HBRUSH ParentBrush(HWND control, HDC dc, const Theme& theme)
{
    const auto brush = reinterpret_cast<HBRUSH>(::SendMessageW(::GetParent(control), WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(dc),
                                                               reinterpret_cast<LPARAM>(control)));  // a WM_CTLCOLOR* result is a brush
    return brush != nullptr ? brush : theme.PanelBrush();
}

struct DcDeleter {
    void operator()(HDC dc) const noexcept { ::DeleteDC(dc); }
};
using UniqueDc = std::unique_ptr<std::remove_pointer_t<HDC>, DcDeleter>;

// Draws into an off-screen bitmap and copies the result to `target` when destroyed, so controls
// that repaint on every hover change do not flicker. Draws straight to `target` if the bitmap
// cannot be created.
class OffscreenPaint {
public:
    OffscreenPaint(HDC target, const RECT& area)
        : target_(target), area_(area), dc_(::CreateCompatibleDC(target)),
          bitmap_(::CreateCompatibleBitmap(target, area.right - area.left, area.bottom - area.top))
    {
        if (!dc_ || !bitmap_) {
            return;
        }
        previous_ = ::SelectObject(dc_.get(), bitmap_.get());
        ::SetViewportOrgEx(dc_.get(), -area_.left, -area_.top, nullptr);
    }
    ~OffscreenPaint()
    {
        if (previous_ == nullptr) {
            return;
        }
        ::BitBlt(target_, area_.left, area_.top, area_.right - area_.left, area_.bottom - area_.top, dc_.get(), area_.left,
                 area_.top, SRCCOPY);
        ::SelectObject(dc_.get(), previous_);
    }
    OffscreenPaint(const OffscreenPaint&) = delete;
    OffscreenPaint& operator=(const OffscreenPaint&) = delete;

    [[nodiscard]] HDC Dc() const noexcept { return previous_ != nullptr ? dc_.get() : target_; }

private:
    HDC target_;
    RECT area_;
    UniqueDc dc_;
    UniqueBitmap bitmap_;
    HGDIOBJ previous_ = nullptr;
};

// ---- List view: the header draws its captions in the theme's dark text unless told otherwise.

LRESULT CALLBACK ListViewProc(HWND list, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
    if (message == WM_NCDESTROY) {
        ::RemoveWindowSubclass(list, &ListViewProc, id);
    } else if (message == WM_NOTIFY && ThemeOf(data).IsDark()) {
        auto& draw = *reinterpret_cast<NMCUSTOMDRAW*>(lParam);  // NM_CUSTOMDRAW carries an NMCUSTOMDRAW
        if (draw.hdr.code == NM_CUSTOMDRAW && draw.hdr.hwndFrom == ListView_GetHeader(list)) {
            if (draw.dwDrawStage == CDDS_PREPAINT) {
                return CDRF_NOTIFYITEMDRAW;
            }
            if (draw.dwDrawStage == CDDS_ITEMPREPAINT) {
                ::SetTextColor(draw.hdc, ThemeOf(data).Colors().text);
                return CDRF_DODEFAULT;
            }
        }
    }
    return ::DefSubclassProc(list, message, wParam, lParam);
}

// ---- Check boxes and radio buttons: the glyph comes from the dark theme, the label is drawn here.

bool IsCheckOrRadio(LONG_PTR style)
{
    const LONG_PTR type = style & BS_TYPEMASK;
    const bool checkType = type == BS_CHECKBOX || type == BS_AUTOCHECKBOX || type == BS_3STATE || type == BS_AUTO3STATE ||
                           type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
    return checkType && (style & BS_PUSHLIKE) == 0;
}

void PaintCheckBox(HWND button, HDC dc, const Theme& theme)
{
    RECT client{};
    ::GetClientRect(button, &client);
    ::FillRect(dc, &client, ParentBrush(button, dc, theme));

    const LONG_PTR style = ::GetWindowLongPtrW(button, GWL_STYLE);
    const LONG_PTR type = style & BS_TYPEMASK;
    const bool radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
    const auto state = static_cast<UINT>(::SendMessageW(button, BM_GETSTATE, 0, 0));
    const bool enabled = ::IsWindowEnabled(button) != FALSE;
    const bool checked = (state & BST_CHECKED) != 0;
    const bool mixed = !radio && (state & BST_INDETERMINATE) != 0;
    // CBS_* and RBS_* share this numbering: four states each for unchecked, checked and mixed.
    int stateId = checked ? CBS_CHECKEDNORMAL : (mixed ? CBS_MIXEDNORMAL : CBS_UNCHECKEDNORMAL);
    if (!enabled) {
        stateId += 3;
    } else if ((state & BST_PUSHED) != 0) {
        stateId += 2;
    } else if ((state & BST_HOT) != 0) {
        stateId += 1;
    }
    const int part = radio ? BP_RADIOBUTTON : BP_CHECKBOX;
    const UINT dpi = DpiOf(button);
    SIZE glyph{Scale(13, dpi), Scale(13, dpi)};
    const HTHEME data = ::OpenThemeDataForDpi(button, VSCLASS_BUTTON, dpi);
    if (data != nullptr) {
        (void)::GetThemePartSize(data, dc, part, stateId, nullptr, TS_DRAW, &glyph);
    }
    const int glyphTop = (client.top + client.bottom - glyph.cy) / 2;
    RECT box{client.left, glyphTop, client.left + glyph.cx, glyphTop + glyph.cy};
    if (data != nullptr) {
        ::DrawThemeBackground(data, dc, part, stateId, &box, nullptr);
        ::CloseThemeData(data);
    } else {
        const UINT kind = radio ? DFCS_BUTTONRADIO : DFCS_BUTTONCHECK;
        ::DrawFrameControl(dc, &box, DFC_BUTTON, kind | (checked ? DFCS_CHECKED : 0) | (enabled ? 0 : DFCS_INACTIVE));
    }

    const std::wstring text = WindowText(button);
    if (text.empty()) {
        return;
    }
    const Cues cues = QueryCues(button);
    SelectObjectScope font(dc, ControlFont(button));
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, enabled ? theme.Colors().text : theme.Colors().mutedText);
    UINT flags = DT_LEFT | DT_NOCLIP | ((style & BS_MULTILINE) != 0 ? DT_WORDBREAK : DT_SINGLELINE);
    if (cues.hideAccelerators) {
        flags |= DT_HIDEPREFIX;
    }
    // Half an average character between glyph and label, where Windows puts it.
    TEXTMETRICW metrics{};
    ::GetTextMetricsW(dc, &metrics);
    const RECT area{box.right + metrics.tmAveCharWidth / 2, client.top, client.right, client.bottom};
    RECT label = area;
    ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &label, flags | DT_CALCRECT);
    label.right = std::min(label.right, area.right);
    ::OffsetRect(&label, 0, ((area.bottom - area.top) - (label.bottom - label.top)) / 2);
    ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &label, flags);
    if (::GetFocus() == button && !cues.hideFocus) {
        RECT focus = label;
        ::InflateRect(&focus, 1, 1);
        ::DrawFocusRect(dc, &focus);
    }
}

LRESULT CALLBACK CheckBoxProc(HWND button, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
    const Theme& theme = ThemeOf(data);
    switch (message) {
    case WM_NCDESTROY:
        ::RemoveWindowSubclass(button, &CheckBoxProc, id);
        break;
    case WM_ERASEBKGND:
        if (theme.IsDark()) {
            return TRUE;  // WM_PAINT fills the whole control
        }
        break;
    case WM_PAINT:
        if (theme.IsDark()) {
            PAINTSTRUCT paint{};
            const HDC dc = ::BeginPaint(button, &paint);
            RECT client{};
            ::GetClientRect(button, &client);
            {
                const OffscreenPaint buffer(dc, client);
                PaintCheckBox(button, buffer.Dc(), theme);
            }
            ::EndPaint(button, &paint);
            return 0;
        }
        break;
    case WM_PRINTCLIENT:
        if (theme.IsDark()) {
            PaintCheckBox(button, reinterpret_cast<HDC>(wParam), theme);  // WM_PRINTCLIENT passes the DC
            return 0;
        }
        break;
    case WM_UPDATEUISTATE: {
        // Underlines and the focus rectangle appear after keyboard use; the label is drawn here.
        const LRESULT result = ::DefSubclassProc(button, message, wParam, lParam);
        if (theme.IsDark()) {
            ::InvalidateRect(button, nullptr, FALSE);
        }
        return result;
    }
    default:
        break;
    }
    return ::DefSubclassProc(button, message, wParam, lParam);
}

// ---- Group boxes: the frame and caption are drawn here; the inside belongs to the controls in it.

void PaintGroupBox(HWND box, HDC dc, const Theme& theme)
{
    RECT client{};
    ::GetClientRect(box, &client);
    const UINT dpi = DpiOf(box);
    SelectObjectScope font(dc, ControlFont(box));
    TEXTMETRICW metrics{};
    ::GetTextMetricsW(dc, &metrics);
    RECT frame = client;
    frame.top += metrics.tmHeight / 2;
    UniqueBrush border(::CreateSolidBrush(theme.Colors().listBorder));
    ::FrameRect(dc, &frame, border.get());

    const std::wstring text = WindowText(box);
    if (text.empty()) {
        return;
    }
    UINT flags = DT_LEFT | DT_SINGLELINE;
    if (QueryCues(box).hideAccelerators) {
        flags |= DT_HIDEPREFIX;
    }
    const RECT area{client.left + Scale(8, dpi), client.top, client.right - Scale(8, dpi), client.top + metrics.tmHeight};
    RECT caption = area;
    ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &caption, flags | DT_CALCRECT);
    caption.right = std::min(caption.right, area.right);
    RECT gap = caption;
    ::InflateRect(&gap, Scale(2, dpi), 0);
    ::FillRect(dc, &gap, ParentBrush(box, dc, theme));
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, ::IsWindowEnabled(box) != FALSE ? theme.Colors().text : theme.Colors().mutedText);
    ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &caption, flags | DT_END_ELLIPSIS);
}

LRESULT CALLBACK GroupBoxProc(HWND box, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
    if (message == WM_NCDESTROY) {
        ::RemoveWindowSubclass(box, &GroupBoxProc, id);
    } else if (message == WM_PAINT && ThemeOf(data).IsDark()) {
        PAINTSTRUCT paint{};
        const HDC dc = ::BeginPaint(box, &paint);
        PaintGroupBox(box, dc, ThemeOf(data));
        ::EndPaint(box, &paint);
        return 0;
    }
    return ::DefSubclassProc(box, message, wParam, lParam);
}

// ---- Tab controls: no dark theme exists for them, so the strip and the page frame are drawn here.

void PaintTab(HWND tab, HDC dc, const Theme& theme)
{
    const Palette& colors = theme.Colors();
    const UINT dpi = DpiOf(tab);
    RECT client{};
    ::GetClientRect(tab, &client);
    ::FillRect(dc, &client, theme.PanelBrush());

    const int count = TabCtrl_GetItemCount(tab);
    const int selected = TabCtrl_GetCurSel(tab);
    // The page starts below the lowest tab: with several rows of tabs, the first one may not be there.
    int stripBottom = client.top;
    for (int i = 0; i < count; ++i) {
        RECT item{};
        if (TabCtrl_GetItemRect(tab, i, &item)) {
            stripBottom = std::max(stripBottom, static_cast<int>(item.bottom));
        }
    }
    UniqueBrush border(::CreateSolidBrush(colors.listBorder));
    UniqueBrush idle(::CreateSolidBrush(colors.window));
    const RECT page{client.left, stripBottom, client.right, client.bottom};
    ::FrameRect(dc, &page, border.get());

    SelectObjectScope font(dc, ControlFont(tab));
    ::SetBkMode(dc, TRANSPARENT);
    const Cues cues = QueryCues(tab);
    for (int i = 0; i < count; ++i) {
        RECT item{};
        if (!TabCtrl_GetItemRect(tab, i, &item)) {
            continue;
        }
        const bool isSelected = i == selected;
        if (isSelected) {
            // The selected tab stands taller and opens into the page below it.
            item.top = std::max(client.top, item.top - Scale(2, dpi));
            item.bottom += 1;
            ::FillRect(dc, &item, theme.PanelBrush());
            ::FrameRect(dc, &item, border.get());
            const RECT opening{item.left + 1, item.bottom - 1, item.right - 1, item.bottom};
            ::FillRect(dc, &opening, theme.PanelBrush());
        } else {
            ::FillRect(dc, &item, idle.get());
            const RECT outline{item.left, item.top, item.right + 1, item.bottom};  // shares its edge with the next tab
            ::FrameRect(dc, &outline, border.get());
        }
        std::array<wchar_t, 128> text{};
        TCITEMW info{};
        info.mask = TCIF_TEXT;
        info.pszText = text.data();
        info.cchTextMax = static_cast<int>(text.size());
        TabCtrl_GetItem(tab, i, &info);
        ::SetTextColor(dc, isSelected ? colors.text : colors.mutedText);
        UINT flags = DT_CENTER | DT_VCENTER | DT_SINGLELINE;
        if (cues.hideAccelerators) {
            flags |= DT_HIDEPREFIX;
        }
        ::DrawTextW(dc, text.data(), -1, &item, flags);
        if (isSelected && ::GetFocus() == tab && !cues.hideFocus) {
            RECT focus = item;
            ::InflateRect(&focus, -Scale(3, dpi), -Scale(3, dpi));
            ::DrawFocusRect(dc, &focus);
        }
    }
}

LRESULT CALLBACK TabProc(HWND tab, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
    const Theme& theme = ThemeOf(data);
    switch (message) {
    case WM_NCDESTROY:
        ::RemoveWindowSubclass(tab, &TabProc, id);
        break;
    case WM_ERASEBKGND:
        if (theme.IsDark()) {
            return TRUE;  // WM_PAINT fills the whole control
        }
        break;
    case WM_PAINT:
        if (theme.IsDark()) {
            PAINTSTRUCT paint{};
            const HDC dc = ::BeginPaint(tab, &paint);
            RECT client{};
            ::GetClientRect(tab, &client);
            {
                const OffscreenPaint buffer(dc, client);
                PaintTab(tab, buffer.Dc(), theme);
            }
            ::EndPaint(tab, &paint);
            return 0;
        }
        break;
    case WM_UPDATEUISTATE: {
        const LRESULT result = ::DefSubclassProc(tab, message, wParam, lParam);
        if (theme.IsDark()) {
            ::InvalidateRect(tab, nullptr, FALSE);
        }
        return result;
    }
    default:
        break;
    }
    return ::DefSubclassProc(tab, message, wParam, lParam);
}

// ---- Static text: Windows embosses disabled text for a light background, so it is drawn here.

bool IsTextStatic(LONG_PTR style)
{
    const LONG_PTR type = style & SS_TYPEMASK;
    return type == SS_LEFT || type == SS_CENTER || type == SS_RIGHT || type == SS_SIMPLE || type == SS_LEFTNOWORDWRAP;
}

void PaintDisabledStatic(HWND label, HDC dc, const Theme& theme)
{
    RECT client{};
    ::GetClientRect(label, &client);
    ::FillRect(dc, &client, ParentBrush(label, dc, theme));
    const LONG_PTR style = ::GetWindowLongPtrW(label, GWL_STYLE);
    const LONG_PTR type = style & SS_TYPEMASK;
    UINT flags = DT_EXPANDTABS;
    if (type == SS_CENTER) {
        flags |= DT_CENTER;
    } else if (type == SS_RIGHT) {
        flags |= DT_RIGHT;
    }
    if ((style & SS_CENTERIMAGE) != 0) {
        flags |= DT_SINGLELINE | DT_VCENTER;
    } else if (type == SS_LEFT || type == SS_CENTER || type == SS_RIGHT) {
        flags |= DT_WORDBREAK;
    }
    if ((style & SS_NOPREFIX) != 0) {
        flags |= DT_NOPREFIX;
    } else if (QueryCues(label).hideAccelerators) {
        flags |= DT_HIDEPREFIX;
    }
    switch (style & SS_ELLIPSISMASK) {
    case SS_ENDELLIPSIS:
        flags |= DT_END_ELLIPSIS;
        break;
    case SS_PATHELLIPSIS:
        flags |= DT_PATH_ELLIPSIS;
        break;
    case SS_WORDELLIPSIS:
        flags |= DT_WORD_ELLIPSIS;
        break;
    default:
        break;
    }
    const std::wstring text = WindowText(label);
    SelectObjectScope font(dc, ControlFont(label));
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, theme.Colors().mutedText);
    ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &client, flags);
}

LRESULT CALLBACK StaticProc(HWND label, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
    const Theme& theme = ThemeOf(data);
    switch (message) {
    case WM_NCDESTROY:
        ::RemoveWindowSubclass(label, &StaticProc, id);
        break;
    case WM_PAINT:
        if (theme.IsDark() && ::IsWindowEnabled(label) == FALSE) {
            PAINTSTRUCT paint{};
            const HDC dc = ::BeginPaint(label, &paint);
            PaintDisabledStatic(label, dc, theme);
            ::EndPaint(label, &paint);
            return 0;
        }
        break;
    case WM_ENABLE: {
        const LRESULT result = ::DefSubclassProc(label, message, wParam, lParam);
        ::InvalidateRect(label, nullptr, TRUE);
        return result;
    }
    default:
        break;
    }
    return ::DefSubclassProc(label, message, wParam, lParam);
}

}  // namespace

void AttachDarkPainting(HWND control, const Theme& theme)
{
    wchar_t className[64] = {};
    ::GetClassNameW(control, className, static_cast<int>(std::size(className)));
    const LONG_PTR style = ::GetWindowLongPtrW(control, GWL_STYLE);
    SUBCLASSPROC proc = nullptr;
    if (ClassIs(className, WC_LISTVIEWW)) {
        proc = &ListViewProc;
    } else if (ClassIs(className, WC_TABCONTROLW)) {
        proc = &TabProc;
    } else if (ClassIs(className, WC_BUTTONW)) {
        if ((style & BS_TYPEMASK) == BS_GROUPBOX) {
            proc = &GroupBoxProc;
        } else if (IsCheckOrRadio(style)) {
            proc = &CheckBoxProc;
        }
    } else if (ClassIs(className, WC_STATICW) && IsTextStatic(style)) {
        proc = &StaticProc;
    }
    if (proc == nullptr) {
        return;
    }
    // The same procedure and id again only replaces the reference data.
    ::SetWindowSubclass(control, proc, kSubclassId, reinterpret_cast<DWORD_PTR>(&theme));
    ::InvalidateRect(control, nullptr, TRUE);
}

}  // namespace ldm::ui
