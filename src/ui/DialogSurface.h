#pragma once

#include "ui/Handles.h"
#include "ui/Theme.h"

#include <windows.h>

#include <initializer_list>
#include <map>
#include <string>
#include <vector>

namespace ldm::ui {

// What a button does in a dialog drawn by DialogSurface. All are flat: a solid fill, no border.
enum class ButtonRole {
    Primary,    // the main action: accent fill
    Secondary,  // other actions: a light neutral fill
    Quiet,      // the least important action: no fill until the mouse is over it
    Link,       // text in the link color, underlined under the mouse
    Picker,     // a value to choose: a field-like box with an icon, the value and a drop-down arrow
};

// How a control's text reads.
enum class TextRole {
    Heading,  // larger and semibold: the file name
    Muted,    // labels: the normal size, muted
    Small,    // secondary information and links: smaller
    Caption,  // smaller and muted
    Success,  // smaller, semibold and green
    Error,    // smaller and red
};

// The file shown on a tile: a page with a badge for its kind, or its extension when it has none.
struct FileTile {
    wchar_t badge = 0;    // an icon-font glyph; 0 for none
    std::wstring label;   // shown when there is no badge, such as "PDF"
    bool completed = false;  // a green check on the tile's corner
};

// The surface's colors, in light or dark mode.
struct SurfaceColors {
    COLORREF content;
    COLORREF footer;
    COLORREF panel;
    COLORREF field;
    COLORREF fieldBorder;
    COLORREF fieldBorderHot;
    COLORREF text;
    COLORREF muted;
    COLORREF success;
    COLORREF error;
    COLORREF link;
    COLORREF primary;
    COLORREF primaryHot;
    COLORREF primaryPressed;
    COLORREF primaryText;
    COLORREF secondary;
    COLORREF secondaryHot;
    COLORREF secondaryPressed;
    COLORREF quietHot;
    COLORREF quietPressed;
    COLORREF disabledFill;
    COLORREF disabledText;
    COLORREF chip;
    COLORREF tile;
    COLORREF page;      // the file's page on a tile
    COLORREF pageLine;
    COLORREF badge;     // the kind badge on the page
    COLORREF check;     // the completed check
};

[[nodiscard]] SurfaceColors SurfaceColorsFor(bool dark) noexcept;

// The look of LDM's redesigned dialogs, added to a DIALOGEX dialog without changing its controls:
// a content area, a footer band along the bottom, rounded panels behind groups of fields, flat
// buttons, pickers, drawn text boxes, pill-shaped tags and a file tile. Text in it uses Segoe UI
// Variable where Windows has it (Windows 11). The dialog owns one, attaches it in OnInit and
// passes it its messages (Dialog::UseSurface does the passing). UI thread only.
class DialogSurface {
public:
    explicit DialogSurface(const Theme& theme) : theme_(theme) {}
    DialogSurface(const DialogSurface&) = delete;
    DialogSurface& operator=(const DialogSurface&) = delete;

    // Sets every control's font, so call it before the roles and fields.
    void Attach(HWND dialog);
    // The footer band: the bottom `dialogUnits` of the dialog, where the buttons sit.
    void SetFooterHeight(int dialogUnits);
    // A rounded panel where the (hidden) control `placeholderId` lies in the template.
    void AddPanel(int placeholderId);
    // Draws the push button `id` itself. The Primary button becomes the dialog's default.
    void AddButton(int id, ButtonRole role);
    // Shows the icon-font `glyph` on button `id` instead of its text (which stays its name).
    void SetButtonGlyph(int id, wchar_t glyph);
    // The icon before a picker's value.
    void SetButtonIcon(int id, wchar_t glyph);
    // For picker `id`'s click: true when it should open its menu (the mouse released it on the
    // drop-down arrow, or the keyboard pressed it); false when the mouse clicked its value.
    [[nodiscard]] bool MenuRequested(int id) const;
    // Draws a rounded box `height96` pixels high (at 96 DPI) around the borderless edit `id`, its
    // text centred in it, outlined in the accent color while it has the focus. The edit's template
    // rectangle is the box; the edit itself shrinks to the line of text inside it.
    void AddField(int id, int height96);
    // Draws the static `id` as a pill around its text.
    void AddChip(int id);
    // Draws the static `id` as the icon-font `glyph` in the muted color.
    void AddIcon(int id, wchar_t glyph);
    void SetTextRole(int id, TextRole role);
    // Draws the static `id` as a tile showing `tile`.
    void SetFileTile(int id, FileTile tile);
    // Lines the controls up from the first one's left edge, each as wide as its text, `gap`
    // apart; the last one takes what is left up to the row's right edge. Hidden ones are skipped.
    void ArrangeRow(std::initializer_list<int> ids, int gapDialogUnits);

    // WM_CTLCOLOR*: the brush to return, or null to leave the message to the dialog.
    [[nodiscard]] HBRUSH CtlColor(UINT message, HDC dc, HWND control);
    // Other messages; true when handled, with `result` set.
    bool HandleMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);

private:
    struct Button {
        ButtonRole role = ButtonRole::Secondary;
        bool hot = false;    // under the mouse
        wchar_t glyph = 0;  // shown instead of the text; 0: the text
        wchar_t icon = 0;   // a picker's icon; 0: none
        int lastClickX = -1;  // where the mouse last released it, in its client area; -1: the keyboard
    };
    struct Field {
        int id = 0;
        int height96 = 0;
    };
    // A row ArrangeRow laid out, kept to lay it out again after a DPI change.
    struct Row {
        std::vector<int> ids;
        int gapDialogUnits = 0;
    };

    static LRESULT CALLBACK ButtonProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                       DWORD_PTR data);
    static LRESULT CALLBACK FieldProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                      DWORD_PTR data);
    void Arrange(const Row& row);
    // Fits the edit to one line of its font, centred where it is.
    void FitField(const Field& field);
    [[nodiscard]] RECT FieldBox(const Field& field) const;
    void RedrawArea(const RECT& rect);
    void RefreshColors();
    void CreateFonts();
    void ApplyFonts();
    void PaintBackground(HDC dc);
    void DrawButton(const DRAWITEMSTRUCT& item, const Button& button);
    void DrawPicker(const DRAWITEMSTRUCT& item, const Button& button);
    void DrawChip(const DRAWITEMSTRUCT& item);
    void DrawIcon(const DRAWITEMSTRUCT& item, wchar_t glyph);
    void DrawTile(const DRAWITEMSTRUCT& item, const FileTile& tile);
    void DrawGlyph(HDC dc, HFONT font, wchar_t glyph, RECT rect, COLORREF color) const;
    [[nodiscard]] RECT ClientRectOf(HWND control) const;
    [[nodiscard]] int FooterTop() const;
    [[nodiscard]] COLORREF BackgroundAt(const RECT& rect) const;
    [[nodiscard]] HBRUSH BrushFor(COLORREF color) const;
    [[nodiscard]] HFONT FontFor(TextRole role) const;
    [[nodiscard]] int TextWidth(HWND control) const;
    [[nodiscard]] int Px(int value96) const;

    const Theme& theme_;
    HWND dialog_ = nullptr;
    int footerDialogUnits_ = 0;
    std::vector<int> panels_;
    std::map<int, Button> buttons_;
    std::vector<int> chips_;
    std::map<int, wchar_t> icons_;
    std::vector<Row> rows_;
    std::vector<Field> fields_;
    std::map<int, TextRole> roles_;
    std::map<int, FileTile> tiles_;
    bool colorsDark_ = false;
    bool colorsReady_ = false;
    SurfaceColors colors_{};
    UniqueBrush contentBrush_;
    UniqueBrush footerBrush_;
    UniqueBrush panelBrush_;
    UniqueBrush fieldBrush_;
    UniqueFont bodyFont_;         // every control without a role
    UniqueFont headingFont_;
    UniqueFont strongFont_;       // the primary button
    UniqueFont smallFont_;        // Small, Caption, Error and the tags
    UniqueFont smallStrongFont_;  // Success
    UniqueFont glyphFont_;        // button glyphs and icons
    UniqueFont chevronFont_;      // a picker's drop-down arrow
};

}  // namespace ldm::ui
