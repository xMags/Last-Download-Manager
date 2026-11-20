#pragma once

#include "ui/Handles.h"
#include "ui/Theme.h"

#include <windows.h>

#include <initializer_list>
#include <map>
#include <vector>

namespace ldm::ui {

// What a button does in a dialog drawn by DialogSurface. All are flat: a solid fill, no border.
enum class ButtonRole {
    Primary,    // the main action: accent fill
    Secondary,  // other actions: a light neutral fill
    Quiet,      // the least important action: no fill until the mouse is over it
    Link,       // text in the link color, underlined under the mouse
};

// How a static text (or a read-only edit) reads.
enum class TextRole {
    Heading,  // larger and semibold: the file name
    Muted,    // secondary information
    Caption,  // small and muted: hints under icons
    Success,  // semibold, green
    Error,    // red
};

// The surface's colors, in light or dark mode.
struct SurfaceColors {
    COLORREF content;
    COLORREF footer;
    COLORREF panel;
    COLORREF field;
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
    COLORREF badge;
};

[[nodiscard]] SurfaceColors SurfaceColorsFor(bool dark) noexcept;

// The look of LDM's redesigned dialogs, added to a DIALOGEX dialog without changing its controls:
// a content area, a footer band along the bottom, rounded panels behind groups of fields, flat
// buttons, pill-shaped tags and a file tile. Text boxes, drop-downs and check boxes stay native
// Windows controls. The dialog owns one, attaches it in OnInit and passes it its messages
// (Dialog::UseSurface does the passing). UI thread only.
class DialogSurface {
public:
    explicit DialogSurface(const Theme& theme) : theme_(theme) {}
    DialogSurface(const DialogSurface&) = delete;
    DialogSurface& operator=(const DialogSurface&) = delete;

    void Attach(HWND dialog);
    // The footer band: the bottom `dialogUnits` of the dialog, where the buttons sit.
    void SetFooterHeight(int dialogUnits);
    // A rounded panel where the (hidden) control `placeholderId` lies in the template.
    void AddPanel(int placeholderId);
    // Draws the push button `id` itself. The Primary button becomes the dialog's default.
    void AddButton(int id, ButtonRole role);
    // Draws the static `id` as a pill around its text.
    void AddChip(int id);
    void SetTextRole(int id, TextRole role);
    // Draws the static `id` as a tile holding `icon` (not owned; keep it alive), with a green
    // check badge when `completed`.
    void SetFileTile(int id, HICON icon, bool completed);
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
        bool hot = false;  // under the mouse
    };
    struct Tile {
        HICON icon = nullptr;
        bool completed = false;
    };
    // A row ArrangeRow laid out, kept to lay it out again after a DPI change.
    struct Row {
        std::vector<int> ids;
        int gapDialogUnits = 0;
    };

    static LRESULT CALLBACK ButtonProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                       DWORD_PTR data);
    void Arrange(const Row& row);
    void RefreshColors();
    void ApplyFonts();
    void PaintBackground(HDC dc);
    void DrawButton(const DRAWITEMSTRUCT& item, const Button& button);
    void DrawChip(const DRAWITEMSTRUCT& item);
    void DrawTile(const DRAWITEMSTRUCT& item, const Tile& tile);
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
    std::vector<Row> rows_;
    std::map<int, TextRole> roles_;
    std::map<int, Tile> tiles_;
    bool colorsDark_ = false;
    bool colorsReady_ = false;
    SurfaceColors colors_{};
    UniqueBrush contentBrush_;
    UniqueBrush footerBrush_;
    UniqueBrush panelBrush_;
    UniqueBrush fieldBrush_;
    UniqueFont headingFont_;
    UniqueFont strongFont_;
    UniqueFont captionFont_;
};

}  // namespace ldm::ui
