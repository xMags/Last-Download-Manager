#include "ui/DialogSurface.h"

#include "ui/Dpi.h"
#include "ui/GlyphIcons.h"
#include "ui/SmoothShapes.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <optional>
#include <string>

namespace ldm::ui {

namespace {

constexpr UINT_PTR kButtonSubclassId = 0x4C44;  // "LD"
constexpr UINT_PTR kFieldSubclassId = 0x4C45;
constexpr int kButtonRadius96 = 6;
constexpr int kFieldRadius96 = 6;
constexpr int kFieldPadding96 = 10;
constexpr int kGlyph96 = 12;
constexpr int kPanelRadius96 = 8;
constexpr int kTileRadius96 = 12;
constexpr int kChipPadding96 = 8;
constexpr int kTextSlack96 = 3;
constexpr int kBadge96 = 24;
constexpr int kBadgeOverhang96 = 8;
constexpr int kTileIconMax96 = 48;

// Posted after a DPI change: Windows gives every control the dialog's rescaled font, so the
// role fonts go back on once it has.
UINT FontsMessage()
{
    static const UINT message = ::RegisterWindowMessageW(L"LdmDialogSurfaceFonts");
    return message;
}

std::wstring WindowText(HWND window)
{
    std::wstring text(static_cast<size_t>(::GetWindowTextLengthW(window)) + 1, L'\0');
    text.resize(static_cast<size_t>(::GetWindowTextW(window, text.data(), static_cast<int>(text.size()))));
    return text;
}

// `base` with its height scaled by `percent` and the given weight.
UniqueFont DeriveFont(HFONT base, int percent, LONG weight)
{
    LOGFONTW font{};
    if (base == nullptr || ::GetObjectW(base, sizeof(font), &font) == 0) {
        return {};
    }
    font.lfHeight = ::MulDiv(font.lfHeight, percent, 100);
    font.lfWeight = weight;
    return UniqueFont(::CreateFontIndirectW(&font));
}

}  // namespace

SurfaceColors SurfaceColorsFor(bool dark) noexcept
{
    if (dark) {
        return SurfaceColors{
            RGB(0x26, 0x26, 0x26), RGB(0x1F, 0x1F, 0x1F), RGB(0x2E, 0x2E, 0x2E), RGB(0x2C, 0x2C, 0x2C),
            RGB(0x4A, 0x4A, 0x4A), RGB(0xFF, 0xFF, 0xFF), RGB(0xC5, 0xC5, 0xC5), RGB(0x6C, 0xCB, 0x5F), RGB(0xFF, 0x99, 0xA4),
            RGB(0x8F, 0xB4, 0xFF), RGB(0x8F, 0xB4, 0xFF), RGB(0xA6, 0xC4, 0xFF), RGB(0x7C, 0xA2, 0xF0),
            RGB(0x0A, 0x15, 0x30), RGB(0x3A, 0x3A, 0x3A), RGB(0x45, 0x45, 0x45), RGB(0x33, 0x33, 0x33),
            RGB(0x35, 0x35, 0x35), RGB(0x2F, 0x2F, 0x2F), RGB(0x33, 0x33, 0x33), RGB(0x8A, 0x8A, 0x8A),
            RGB(0x3A, 0x3A, 0x3A), RGB(0x23, 0x31, 0x4F), RGB(0x13, 0xA1, 0x0E),
        };
    }
    return SurfaceColors{
        RGB(0xFF, 0xFF, 0xFF), RGB(0xF3, 0xF3, 0xF3), RGB(0xF6, 0xF6, 0xF6), RGB(0xFF, 0xFF, 0xFF),
        RGB(0xD4, 0xD4, 0xD4), RGB(0x1A, 0x1A, 0x1A), RGB(0x5C, 0x5C, 0x5C), RGB(0x0E, 0x7A, 0x0D), RGB(0xC4, 0x2B, 0x1C),
        RGB(0x25, 0x59, 0xD9), RGB(0x25, 0x59, 0xD9), RGB(0x1F, 0x4F, 0xC8), RGB(0x1A, 0x44, 0xAD),
        RGB(0xFF, 0xFF, 0xFF), RGB(0xE8, 0xE8, 0xE8), RGB(0xDE, 0xDE, 0xDE), RGB(0xD2, 0xD2, 0xD2),
        RGB(0xEB, 0xEB, 0xEB), RGB(0xE0, 0xE0, 0xE0), RGB(0xEE, 0xEE, 0xEE), RGB(0x9E, 0x9E, 0x9E),
        RGB(0xED, 0xED, 0xED), RGB(0xEE, 0xF3, 0xFF), RGB(0x13, 0xA1, 0x0E),
    };
}

void DialogSurface::Attach(HWND dialog)
{
    dialog_ = dialog;
    RefreshColors();
}

void DialogSurface::SetFooterHeight(int dialogUnits)
{
    footerDialogUnits_ = dialogUnits;
}

void DialogSurface::AddPanel(int placeholderId)
{
    panels_.push_back(placeholderId);
}

void DialogSurface::AddButton(int id, ButtonRole role)
{
    const HWND button = ::GetDlgItem(dialog_, id);
    if (button == nullptr) {
        return;
    }
    buttons_[id] = Button{role, false};
    const LONG_PTR style = ::GetWindowLongPtrW(button, GWL_STYLE);
    ::SetWindowLongPtrW(button, GWL_STYLE, (style & ~static_cast<LONG_PTR>(BS_TYPEMASK)) | BS_OWNERDRAW);
    ::SetWindowSubclass(button, &DialogSurface::ButtonProc, kButtonSubclassId, reinterpret_cast<DWORD_PTR>(this));
    if (role == ButtonRole::Primary) {
        ::SendMessageW(dialog_, DM_SETDEFID, static_cast<WPARAM>(id), 0);
        if (strongFont_ == nullptr) {
            ApplyFonts();
        } else {
            ::SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(strongFont_.get()), TRUE);
        }
    }
    ::InvalidateRect(button, nullptr, TRUE);
}

void DialogSurface::AddChip(int id)
{
    const HWND chip = ::GetDlgItem(dialog_, id);
    if (chip == nullptr) {
        return;
    }
    chips_.push_back(id);
    const LONG_PTR style = ::GetWindowLongPtrW(chip, GWL_STYLE);
    ::SetWindowLongPtrW(chip, GWL_STYLE, (style & ~static_cast<LONG_PTR>(SS_TYPEMASK)) | SS_OWNERDRAW);
    SetTextRole(id, TextRole::Small);
    ::InvalidateRect(chip, nullptr, TRUE);
}

void DialogSurface::SetButtonGlyph(int id, wchar_t glyph)
{
    if (const auto button = buttons_.find(id); button != buttons_.end()) {
        button->second.glyph = glyph;
        ::InvalidateRect(::GetDlgItem(dialog_, id), nullptr, TRUE);
    }
}

void DialogSurface::AddField(int id, int height96)
{
    const HWND edit = ::GetDlgItem(dialog_, id);
    if (edit == nullptr) {
        return;
    }
    // The box is drawn here, so the edit loses its own border, and its width the box's padding.
    ::SetWindowLongPtrW(edit, GWL_STYLE, ::GetWindowLongPtrW(edit, GWL_STYLE) & ~static_cast<LONG_PTR>(WS_BORDER));
    ::SetWindowLongPtrW(edit, GWL_EXSTYLE,
                        ::GetWindowLongPtrW(edit, GWL_EXSTYLE) & ~static_cast<LONG_PTR>(WS_EX_CLIENTEDGE));
    const RECT box = ClientRectOf(edit);
    const int padding = Px(kFieldPadding96);
    ::SetWindowPos(edit, nullptr, box.left + padding, box.top, std::max<LONG>(0, box.right - box.left - 2 * padding),
                   box.bottom - box.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    fields_.push_back(Field{id, height96});
    FitField(fields_.back());
    ::SetWindowSubclass(edit, &DialogSurface::FieldProc, kFieldSubclassId, reinterpret_cast<DWORD_PTR>(this));
}

void DialogSurface::FitField(const Field& field)
{
    const HWND edit = ::GetDlgItem(dialog_, field.id);
    if (edit == nullptr) {
        return;
    }
    TEXTMETRICW metrics{};
    const HDC dc = ::GetDC(edit);
    if (dc == nullptr) {
        return;
    }
    {
        const SelectObjectScope selectFont(dc, reinterpret_cast<HFONT>(::SendMessageW(edit, WM_GETFONT, 0, 0)));
        ::GetTextMetricsW(dc, &metrics);
    }
    ::ReleaseDC(edit, dc);
    const RECT rect = ClientRectOf(edit);
    const LONG middle = (rect.top + rect.bottom) / 2;
    const LONG height = metrics.tmHeight + 2;
    ::SetWindowPos(edit, nullptr, rect.left, middle - height / 2, rect.right - rect.left, height,
                   SWP_NOZORDER | SWP_NOACTIVATE);
}

RECT DialogSurface::FieldBox(const Field& field) const
{
    const RECT edit = ClientRectOf(::GetDlgItem(dialog_, field.id));
    const LONG middle = (edit.top + edit.bottom) / 2;
    const int padding = Px(kFieldPadding96);
    const int height = Px(field.height96);
    return RECT{edit.left - padding, middle - height / 2, edit.right + padding, middle - height / 2 + height};
}

void DialogSurface::RedrawArea(const RECT& rect)
{
    // The children too: a dialog does not clip them, so its background covers them when erased.
    ::RedrawWindow(dialog_, &rect, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

LRESULT CALLBACK DialogSurface::FieldProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                          DWORD_PTR data)
{
    const LRESULT result = ::DefSubclassProc(window, message, wParam, lParam);
    auto* self = reinterpret_cast<DialogSurface*>(data);  // set by SetWindowSubclass in AddField
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_SHOWWINDOW) {
        const int controlId = ::GetDlgCtrlID(window);
        for (const Field& field : self->fields_) {
            if (field.id == controlId) {
                RECT box = self->FieldBox(field);
                ::InflateRect(&box, 1, 1);
                self->RedrawArea(box);
            }
        }
    } else if (message == WM_NCDESTROY) {
        ::RemoveWindowSubclass(window, &DialogSurface::FieldProc, id);
    }
    return result;
}

void DialogSurface::SetTextRole(int id, TextRole role)
{
    roles_[id] = role;
    ApplyFonts();
}

void DialogSurface::SetFileTile(int id, HICON icon, bool completed)
{
    const HWND tile = ::GetDlgItem(dialog_, id);
    if (tile == nullptr) {
        return;
    }
    tiles_[id] = Tile{icon, completed};
    const LONG_PTR style = ::GetWindowLongPtrW(tile, GWL_STYLE);
    if ((style & SS_TYPEMASK) != SS_OWNERDRAW) {
        ::SetWindowLongPtrW(tile, GWL_STYLE, (style & ~static_cast<LONG_PTR>(SS_TYPEMASK)) | SS_OWNERDRAW);
    }
    ::InvalidateRect(tile, nullptr, TRUE);
}

void DialogSurface::ArrangeRow(std::initializer_list<int> ids, int gapDialogUnits)
{
    std::vector<int> wanted(ids);
    const auto known = std::find_if(rows_.begin(), rows_.end(), [&wanted](const Row& row) { return row.ids == wanted; });
    if (known == rows_.end()) {
        rows_.push_back(Row{wanted, gapDialogUnits});
    }
    Arrange(Row{std::move(wanted), gapDialogUnits});
}

void DialogSurface::Arrange(const Row& wanted)
{
    std::vector<HWND> row;
    LONG rowRight = 0;
    for (const int id : wanted.ids) {
        if (const HWND control = ::GetDlgItem(dialog_, id); control != nullptr) {
            row.push_back(control);
            rowRight = std::max(rowRight, ClientRectOf(control).right);
        }
    }
    if (row.empty()) {
        return;
    }
    RECT gap{0, 0, wanted.gapDialogUnits, 0};
    ::MapDialogRect(dialog_, &gap);
    LONG x = ClientRectOf(row.front()).left;
    for (size_t i = 0; i < row.size(); ++i) {
        const HWND control = row[i];
        // The control's own flag: while a dialog is still hidden (in WM_INITDIALOG),
        // IsWindowVisible says no for every control in it.
        if ((::GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) == 0) {
            continue;
        }
        const RECT rect = ClientRectOf(control);
        const int id = ::GetDlgCtrlID(control);
        const bool chip = std::find(chips_.begin(), chips_.end(), id) != chips_.end();
        // A little more than the measured text: drawing may round a pixel wider than measuring,
        // and a box exactly as wide as its text would then show it cut short.
        LONG width = TextWidth(control) + (chip ? 2 * Px(kChipPadding96) : 0) + Px(kTextSlack96);
        if (i + 1 == row.size()) {
            width = std::max<LONG>(0, std::max(width, rowRight - x));
        }
        width = std::min<LONG>(width, std::max<LONG>(0, rowRight - x));
        ::SetWindowPos(control, nullptr, x, rect.top, width, rect.bottom - rect.top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        ::InvalidateRect(control, nullptr, TRUE);
        x += width + gap.right;
    }
    ::InvalidateRect(dialog_, nullptr, TRUE);
}

HBRUSH DialogSurface::CtlColor(UINT message, HDC dc, HWND control)
{
    RefreshColors();
    switch (message) {
    case WM_CTLCOLORDLG:
        ::SetBkColor(dc, colors_.content);
        return contentBrush_.get();
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        ::SetTextColor(dc, colors_.text);
        ::SetBkColor(dc, colors_.field);
        return fieldBrush_.get();
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        const COLORREF background = BackgroundAt(ClientRectOf(control));
        COLORREF text = colors_.text;
        if (const auto role = roles_.find(::GetDlgCtrlID(control)); role != roles_.end()) {
            switch (role->second) {
            case TextRole::Muted:
            case TextRole::Caption:
                text = colors_.muted;
                break;
            case TextRole::Success:
                text = colors_.success;
                break;
            case TextRole::Error:
                text = colors_.error;
                break;
            case TextRole::Heading:
            case TextRole::Small:
                break;
            }
        }
        ::SetTextColor(dc, text);
        ::SetBkColor(dc, background);
        return BrushFor(background);
    }
    default:
        return nullptr;
    }
}

bool DialogSurface::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
{
    if (dialog_ == nullptr) {
        return false;
    }
    if (message == WM_ERASEBKGND) {
        RefreshColors();
        PaintBackground(reinterpret_cast<HDC>(wParam));  // WM_ERASEBKGND passes the DC
        result = TRUE;
        return true;
    }
    if (message == WM_DRAWITEM) {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);  // WM_DRAWITEM passes it
        const int id = static_cast<int>(item->CtlID);
        RefreshColors();
        if (const auto button = buttons_.find(id); button != buttons_.end() && item->CtlType == ODT_BUTTON) {
            DrawButton(*item, button->second);
        } else if (const auto tile = tiles_.find(id); tile != tiles_.end() && item->CtlType == ODT_STATIC) {
            DrawTile(*item, tile->second);
        } else if (std::find(chips_.begin(), chips_.end(), id) != chips_.end() && item->CtlType == ODT_STATIC) {
            DrawChip(*item);
        } else {
            return false;
        }
        result = TRUE;
        return true;
    }
    if (message == WM_DPICHANGED) {
        ::PostMessageW(dialog_, FontsMessage(), 0, 0);
        return false;  // the dialog manager rescales the dialog and its controls
    }
    if (message == WM_LBUTTONDOWN) {
        // A click in a field's box beside its edit puts the caret in the edit.
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        for (const Field& field : fields_) {
            const HWND edit = ::GetDlgItem(dialog_, field.id);
            const RECT box = FieldBox(field);
            if (edit != nullptr && (::GetWindowLongPtrW(edit, GWL_STYLE) & WS_VISIBLE) != 0 && ::PtInRect(&box, point)) {
                ::SetFocus(edit);
                result = 0;
                return true;
            }
        }
        return false;
    }
    if (message == FontsMessage()) {
        headingFont_.reset();
        strongFont_.reset();
        smallFont_.reset();
        smallStrongFont_.reset();
        glyphFont_.reset();
        ApplyFonts();
        // The text grew or shrank with the new DPI, so the rows are measured again.
        for (const Row& row : rows_) {
            Arrange(row);
        }
        ::InvalidateRect(dialog_, nullptr, TRUE);
        result = 0;
        return true;
    }
    if (message == WM_SETTINGCHANGE || message == WM_THEMECHANGED || message == WM_SYSCOLORCHANGE) {
        ::InvalidateRect(dialog_, nullptr, TRUE);
    }
    return false;
}

LRESULT CALLBACK DialogSurface::ButtonProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                           DWORD_PTR data)
{
    auto* self = reinterpret_cast<DialogSurface*>(data);  // set by SetWindowSubclass in AddButton
    const auto button = self->buttons_.find(::GetDlgCtrlID(window));
    switch (message) {
    case WM_GETDLGCODE:
        // Owner-drawn buttons say they are plain buttons, which would make Enter press the
        // dialog's default button even when another one has the focus. As push buttons, the
        // focused one is pressed, as with standard buttons.
        return DLGC_BUTTON | (::GetFocus() == window ? DLGC_DEFPUSHBUTTON : DLGC_UNDEFPUSHBUTTON);
    case BM_SETSTYLE:
        // The dialog manager moves the default style between push buttons as the focus moves;
        // it would replace the owner-drawn style, and the drawing does not depend on it.
        ::InvalidateRect(window, nullptr, TRUE);
        return 0;
    case WM_MOUSEMOVE:
        if (button != self->buttons_.end() && !button->second.hot) {
            button->second.hot = true;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            ::TrackMouseEvent(&track);
            ::InvalidateRect(window, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        if (button != self->buttons_.end()) {
            button->second.hot = false;
            ::InvalidateRect(window, nullptr, FALSE);
        }
        break;
    case WM_NCDESTROY:
        ::RemoveWindowSubclass(window, &DialogSurface::ButtonProc, id);
        break;
    default:
        break;
    }
    return ::DefSubclassProc(window, message, wParam, lParam);
}

void DialogSurface::RefreshColors()
{
    const bool dark = theme_.IsDark();
    if (colorsReady_ && dark == colorsDark_) {
        return;
    }
    colorsReady_ = true;
    colorsDark_ = dark;
    colors_ = SurfaceColorsFor(dark);
    contentBrush_.reset(::CreateSolidBrush(colors_.content));
    footerBrush_.reset(::CreateSolidBrush(colors_.footer));
    panelBrush_.reset(::CreateSolidBrush(colors_.panel));
    fieldBrush_.reset(::CreateSolidBrush(colors_.field));
}

void DialogSurface::ApplyFonts()
{
    if (dialog_ == nullptr) {
        return;
    }
    const auto base = reinterpret_cast<HFONT>(::SendMessageW(dialog_, WM_GETFONT, 0, 0));
    if (headingFont_ == nullptr) {
        headingFont_ = DeriveFont(base, 125, FW_SEMIBOLD);
        strongFont_ = DeriveFont(base, 100, FW_SEMIBOLD);
        smallFont_ = DeriveFont(base, 90, FW_NORMAL);
        smallStrongFont_ = DeriveFont(base, 90, FW_SEMIBOLD);
        glyphFont_ = CreateIconFont(Px(kGlyph96));
    }
    for (const auto& [id, role] : roles_) {
        if (const HFONT font = FontFor(role); font != nullptr) {
            ::SendDlgItemMessageW(dialog_, id, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
    }
    for (const auto& [id, button] : buttons_) {
        if (button.role == ButtonRole::Primary && strongFont_ != nullptr) {
            ::SendDlgItemMessageW(dialog_, id, WM_SETFONT, reinterpret_cast<WPARAM>(strongFont_.get()), TRUE);
        }
    }
    // A new font changes the height of a field's line of text.
    for (const Field& field : fields_) {
        FitField(field);
    }
}

HFONT DialogSurface::FontFor(TextRole role) const
{
    switch (role) {
    case TextRole::Heading:
        return headingFont_.get();
    case TextRole::Success:
        return smallStrongFont_.get();
    case TextRole::Small:
    case TextRole::Caption:
    case TextRole::Error:
        return smallFont_.get();
    case TextRole::Muted:
        return nullptr;  // the dialog's font
    }
    return nullptr;
}

void DialogSurface::PaintBackground(HDC dc)
{
    RECT client{};
    ::GetClientRect(dialog_, &client);
    ::FillRect(dc, &client, contentBrush_.get());
    if (footerDialogUnits_ > 0) {
        const RECT footer{client.left, FooterTop(), client.right, client.bottom};
        ::FillRect(dc, &footer, footerBrush_.get());
    }
    for (const int id : panels_) {
        if (const HWND placeholder = ::GetDlgItem(dialog_, id); placeholder != nullptr) {
            FillRoundedRect(dc, ClientRectOf(placeholder), Px(kPanelRadius96), colors_.panel);
        }
    }
    for (const Field& field : fields_) {
        const HWND edit = ::GetDlgItem(dialog_, field.id);
        if (edit == nullptr || (::GetWindowLongPtrW(edit, GWL_STYLE) & WS_VISIBLE) == 0) {
            continue;
        }
        const RECT box = FieldBox(field);
        const int radius = Px(kFieldRadius96);
        FillRoundedRect(dc, box, radius, colors_.field);
        const bool focused = ::GetFocus() == edit;
        FrameRoundedRect(dc, box, radius, focused ? std::max(2, Px(2)) : 1, focused ? colors_.primary : colors_.fieldBorder);
    }
}

void DialogSurface::DrawButton(const DRAWITEMSTRUCT& item, const Button& button)
{
    const HDC dc = item.hDC;
    RECT rect = item.rcItem;
    ::FillRect(dc, &rect, BrushFor(BackgroundAt(ClientRectOf(item.hwndItem))));
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool hot = button.hot && !disabled;
    std::optional<COLORREF> fill;
    COLORREF text = disabled ? colors_.disabledText : colors_.text;
    switch (button.role) {
    case ButtonRole::Primary:
        fill = disabled ? colors_.disabledFill : pressed ? colors_.primaryPressed : hot ? colors_.primaryHot : colors_.primary;
        text = disabled ? colors_.disabledText : colors_.primaryText;
        break;
    case ButtonRole::Secondary:
        fill = disabled ? colors_.disabledFill
                        : pressed ? colors_.secondaryPressed : hot ? colors_.secondaryHot : colors_.secondary;
        break;
    case ButtonRole::Quiet:
        if (pressed) {
            fill = colors_.quietPressed;
        } else if (hot) {
            fill = colors_.quietHot;
        }
        break;
    case ButtonRole::Link:
        text = disabled ? colors_.disabledText : colors_.link;
        break;
    }
    const int radius = Px(kButtonRadius96);
    if (fill) {
        FillRoundedRect(dc, rect, radius, *fill);
    }

    if (button.glyph != 0 && glyphFont_ != nullptr) {
        const SelectObjectScope selectGlyph(dc, glyphFont_.get());
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, text);
        const wchar_t glyph = button.glyph;
        RECT glyphRect = rect;
        ::DrawTextW(dc, &glyph, 1, &glyphRect, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        if ((item.itemState & ODS_FOCUS) != 0 && (item.itemState & ODS_NOFOCUSRECT) == 0) {
            FrameRoundedRect(dc, rect, radius, std::max(2, Px(2)), colors_.text);
        }
        return;
    }

    const std::wstring label = WindowText(item.hwndItem);
    const auto font = reinterpret_cast<HFONT>(::SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
    UniqueFont underlined;
    HFONT drawFont = font;
    if (button.role == ButtonRole::Link && hot) {
        LOGFONTW logFont{};
        if (font != nullptr && ::GetObjectW(font, sizeof(logFont), &logFont) != 0) {
            logFont.lfUnderline = TRUE;
            underlined.reset(::CreateFontIndirectW(&logFont));
            drawFont = underlined.get();
        }
    }
    const SelectObjectScope selectFont(dc, drawFont);
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, text);
    UINT format = DT_SINGLELINE | DT_VCENTER;
    format |= button.role == ButtonRole::Link ? DT_LEFT | DT_PATH_ELLIPSIS : DT_CENTER | DT_END_ELLIPSIS;
    if ((item.itemState & ODS_NOACCEL) != 0) {
        format |= DT_HIDEPREFIX;
    }
    RECT textRect = rect;
    if (button.role != ButtonRole::Link) {
        ::InflateRect(&textRect, -Px(6), 0);
    }
    std::wstring shown = label;  // DT_PATH_ELLIPSIS may change the buffer it is given
    ::DrawTextW(dc, shown.data(), static_cast<int>(shown.size()), &textRect, format);

    if ((item.itemState & ODS_FOCUS) != 0 && (item.itemState & ODS_NOFOCUSRECT) == 0) {
        FrameRoundedRect(dc, rect, radius, std::max(2, Px(2)), colors_.text);
    }
}

void DialogSurface::DrawChip(const DRAWITEMSTRUCT& item)
{
    const HDC dc = item.hDC;
    RECT rect = item.rcItem;
    ::FillRect(dc, &rect, BrushFor(BackgroundAt(ClientRectOf(item.hwndItem))));
    const std::wstring text = WindowText(item.hwndItem);
    if (text.empty()) {
        return;
    }
    FillRoundedRect(dc, rect, (rect.bottom - rect.top) / 2, colors_.chip);
    const auto font = reinterpret_cast<HFONT>(::SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
    const SelectObjectScope selectFont(dc, font);
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, colors_.text);
    RECT textRect = rect;
    ::InflateRect(&textRect, -Px(kChipPadding96), 0);
    ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &textRect,
                DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void DialogSurface::DrawTile(const DRAWITEMSTRUCT& item, const Tile& tile)
{
    const HDC dc = item.hDC;
    const RECT rect = item.rcItem;
    const COLORREF background = BackgroundAt(ClientRectOf(item.hwndItem));
    ::FillRect(dc, &rect, BrushFor(background));
    const int overhang = tile.completed ? Px(kBadgeOverhang96) : 0;
    const int side = std::min(rect.right - rect.left, rect.bottom - rect.top) - overhang;
    if (side <= 0) {
        return;
    }
    const RECT box{rect.left, rect.top, rect.left + side, rect.top + side};
    FillRoundedRect(dc, box, Px(kTileRadius96), colors_.tile);
    if (tile.icon != nullptr) {
        const int iconSize = std::min(Px(kTileIconMax96), side * 2 / 3);
        ::DrawIconEx(dc, box.left + (side - iconSize) / 2, box.top + (side - iconSize) / 2, tile.icon, iconSize, iconSize, 0,
                     nullptr, DI_NORMAL);
    }
    if (!tile.completed) {
        return;
    }
    const int badge = Px(kBadge96);
    const RECT ring{box.right + overhang - badge, box.bottom + overhang - badge, box.right + overhang,
                    box.bottom + overhang};
    FillCircle(dc, ring, background);  // a gap between the badge and the tile
    const int gap = std::max(2, Px(2));
    const RECT disc{ring.left + gap, ring.top + gap, ring.right - gap, ring.bottom - gap};
    FillCircle(dc, disc, colors_.badge);
    const int w = disc.right - disc.left;
    const int h = disc.bottom - disc.top;
    DrawCheckMark(dc, POINT{disc.left + w * 28 / 100, disc.top + h * 52 / 100},
                  POINT{disc.left + w * 44 / 100, disc.top + h * 68 / 100},
                  POINT{disc.left + w * 73 / 100, disc.top + h * 35 / 100}, std::max(2, Px(2)), RGB(0xFF, 0xFF, 0xFF));
}

RECT DialogSurface::ClientRectOf(HWND control) const
{
    RECT rect{};
    ::GetWindowRect(control, &rect);
    ::MapWindowPoints(nullptr, dialog_, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}

int DialogSurface::FooterTop() const
{
    RECT client{};
    ::GetClientRect(dialog_, &client);
    RECT footer{0, 0, 0, footerDialogUnits_};
    ::MapDialogRect(dialog_, &footer);
    return client.bottom - footer.bottom;
}

COLORREF DialogSurface::BackgroundAt(const RECT& rect) const
{
    if (footerDialogUnits_ > 0 && rect.top >= FooterTop()) {
        return colors_.footer;
    }
    const POINT middle{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
    for (const int id : panels_) {
        if (const HWND placeholder = ::GetDlgItem(dialog_, id); placeholder != nullptr) {
            const RECT panel = ClientRectOf(placeholder);
            if (::PtInRect(&panel, middle)) {
                return colors_.panel;
            }
        }
    }
    return colors_.content;
}

HBRUSH DialogSurface::BrushFor(COLORREF color) const
{
    if (color == colors_.footer) {
        return footerBrush_.get();
    }
    if (color == colors_.panel) {
        return panelBrush_.get();
    }
    return contentBrush_.get();
}

int DialogSurface::TextWidth(HWND control) const
{
    const std::wstring text = WindowText(control);
    if (text.empty()) {
        return 0;
    }
    const HDC dc = ::GetDC(control);
    if (dc == nullptr) {
        return 0;
    }
    const auto font = reinterpret_cast<HFONT>(::SendMessageW(control, WM_GETFONT, 0, 0));
    RECT measured{};
    {
        const SelectObjectScope selectFont(dc, font);
        ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &measured, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
    }
    ::ReleaseDC(control, dc);
    return measured.right - measured.left;
}

int DialogSurface::Px(int value96) const
{
    return Scale(value96, DpiOf(dialog_));
}

}  // namespace ldm::ui
