#include "ui/MediaPanelMenuPainter.h"

#include "ui/Dpi.h"
#include "ui/GlyphIcons.h"

#include <algorithm>
#include <string_view>

namespace ldm::ui {

namespace {

// Icon font glyphs (GlyphIcons).
constexpr wchar_t kDownloadGlyph = 0xE896;      // Download
constexpr wchar_t kVideoGlyph = 0xE714;         // Video
constexpr wchar_t kAudioGlyph = 0xE8D6;         // Audio
constexpr wchar_t kSubtitlesGlyph = 0xED1E;     // Subtitles
constexpr wchar_t kSmallerGlyph = 0xE73F;       // BackToWindow
constexpr wchar_t kBiggerGlyph = 0xE740;        // FullScreen
constexpr wchar_t kHideGlyph = 0xED1A;          // Hide
constexpr wchar_t kInfoGlyph = 0xE946;          // Info
constexpr wchar_t kLockGlyph = 0xE72E;          // Lock
constexpr wchar_t kChevronRightGlyph = 0xE76C;  // ChevronRight

// Sizes at 96 DPI.
constexpr int kRowHeight96 = 32;
constexpr int kSeparatorHeight96 = 9;
constexpr int kEdge96 = 12;           // from the menu's edge to the icon, and to the size or chevron
constexpr int kIcon96 = 16;
constexpr int kIconGap96 = 12;        // between the icon and the text
constexpr int kDetailGap96 = 5;       // between "1080p" and its details
constexpr int kBadgeGap96 = 8;
constexpr int kBadgeHeight96 = 16;
constexpr int kBadgePadding96 = 5;
constexpr int kBadgeText96 = 10;
constexpr int kSizeGap96 = 28;        // the least room between the text and the size
constexpr int kChevron96 = 10;
constexpr int kChevronRoom96 = 18;    // a submenu's chevron and the room before it
constexpr int kHighlightInset96 = 4;
constexpr int kHighlightRadius96 = 8;

COLORREF Blend(COLORREF from, COLORREF to, int percent)
{
    const auto mix = [percent](int a, int b) { return a + (b - a) * percent / 100; };
    return RGB(mix(GetRValue(from), GetRValue(to)), mix(GetGValue(from), GetGValue(to)),
               mix(GetBValue(from), GetBValue(to)));
}

wchar_t GlyphOf(PanelGlyph glyph)
{
    switch (glyph) {
    case PanelGlyph::Download:
        return kDownloadGlyph;
    case PanelGlyph::Video:
        return kVideoGlyph;
    case PanelGlyph::Audio:
        return kAudioGlyph;
    case PanelGlyph::Subtitles:
        return kSubtitlesGlyph;
    case PanelGlyph::Smaller:
        return kSmallerGlyph;
    case PanelGlyph::Bigger:
        return kBiggerGlyph;
    case PanelGlyph::Hide:
        return kHideGlyph;
    case PanelGlyph::Info:
        return kInfoGlyph;
    case PanelGlyph::Lock:
        return kLockGlyph;
    case PanelGlyph::None:
        break;
    }
    return L'\0';
}

// The parts of a line's text: "<first> <detail>\t<size>"; detail is empty unless asked for.
struct LineText {
    std::wstring_view first;
    std::wstring_view detail;
    std::wstring_view size;
};

LineText SplitText(const PanelMenuItem& item)
{
    const std::wstring_view text = item.text;
    const size_t tab = text.find(L'\t');
    LineText parts;
    parts.first = text.substr(0, tab);
    if (tab != std::wstring_view::npos) {
        parts.size = text.substr(tab + 1);
    }
    if (item.quietDetail) {
        const size_t space = parts.first.find(L' ');
        if (space != std::wstring_view::npos) {
            parts.detail = parts.first.substr(space + 1);
            parts.first = parts.first.substr(0, space);
        }
    }
    return parts;
}

int TextWidth(HDC dc, HFONT font, std::wstring_view text)
{
    if (text.empty()) {
        return 0;
    }
    const SelectObjectScope select(dc, font);
    SIZE size{};
    ::GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &size);
    return size.cx;
}

void DrawTextAt(HDC dc, HFONT font, COLORREF color, std::wstring_view text, RECT rect, UINT format)
{
    const SelectObjectScope select(dc, font);
    ::SetTextColor(dc, color);
    ::DrawTextW(dc, text.data(), static_cast<int>(text.size()), &rect,
                format | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

void FillRounded(HDC dc, const RECT& rect, int radius, COLORREF color)
{
    UniqueBrush brush(::CreateSolidBrush(color));
    const SelectObjectScope selectBrush(dc, brush.get());
    const SelectObjectScope selectPen(dc, ::GetStockObject(NULL_PEN));
    // A null pen leaves the right and bottom edge out; one more pixel makes up for it.
    ::RoundRect(dc, rect.left, rect.top, rect.right + 1, rect.bottom + 1, radius, radius);
}

const PanelMenuItem* ItemOf(ULONG_PTR data)
{
    return reinterpret_cast<const PanelMenuItem*>(data);  // set by the controller's AppendItems
}

}  // namespace

PanelMenuPainter::PanelMenuPainter(const Theme& theme, UINT dpi)
    : colors_(theme.Colors()),
      dark_(theme.IsDark()),
      dpi_(dpi),
      text_(CreateMessageFont(dpi)),
      icons_(CreateIconFont(Scale(kIcon96, dpi))),
      chevron_(CreateIconFont(Scale(kChevron96, dpi))),
      background_(::CreateSolidBrush(dark_ ? colors_.panel : colors_.window))
{
    LOGFONTW badge{};
    ::GetObjectW(text_.get(), sizeof(badge), &badge);
    badge.lfHeight = -Scale(kBadgeText96, dpi);
    badge.lfWeight = FW_BOLD;
    badgeFont_.reset(::CreateFontIndirectW(&badge));
}

int PanelMenuPainter::Px(int value96) const noexcept
{
    return Scale(value96, dpi_);
}

int PanelMenuPainter::NaturalWidth(const std::vector<PanelMenuItem>& items) const
{
    int widest = 0;
    for (const PanelMenuItem& item : items) {
        if (!item.separator) {
            widest = std::max(widest, MeasureWidth(item));
        }
    }
    return widest;
}

void PanelMenuPainter::Measure(MEASUREITEMSTRUCT& measure) const
{
    const PanelMenuItem* item = ItemOf(measure.itemData);
    if (item == nullptr || item->separator) {
        measure.itemWidth = 0;
        measure.itemHeight = static_cast<UINT>(Px(kSeparatorHeight96));
        return;
    }
    measure.itemWidth = static_cast<UINT>(std::max(MeasureWidth(*item), minimumWidth_));
    measure.itemHeight = static_cast<UINT>(Px(kRowHeight96));
}

int PanelMenuPainter::MeasureWidth(const PanelMenuItem& item) const
{
    const LineText parts = SplitText(item);
    const HDC dc = ::GetDC(nullptr);
    int width = Px(kEdge96 + kIcon96 + kIconGap96) + TextWidth(dc, text_.get(), parts.first);
    if (!parts.detail.empty()) {
        width += Px(kDetailGap96) + TextWidth(dc, text_.get(), parts.detail);
    }
    if (!item.badge.empty()) {
        width += Px(kBadgeGap96 + 2 * kBadgePadding96) + TextWidth(dc, badgeFont_.get(), item.badge);
    }
    if (!parts.size.empty()) {
        width += Px(kSizeGap96) + TextWidth(dc, text_.get(), parts.size);
    }
    ::ReleaseDC(nullptr, dc);
    return width + Px(kEdge96) + (item.children.empty() ? 0 : Px(kChevronRoom96));
}

void PanelMenuPainter::Draw(const DRAWITEMSTRUCT& draw) const
{
    const HDC dc = draw.hDC;
    const RECT row = draw.rcItem;
    const COLORREF background = dark_ ? colors_.panel : colors_.window;
    ::FillRect(dc, &row, background_.get());
    const PanelMenuItem* item = ItemOf(draw.itemData);
    if (item == nullptr || item->separator) {
        UniqueBrush line(::CreateSolidBrush(colors_.divider));
        const int middle = (row.top + row.bottom) / 2;
        const RECT rule{row.left + Px(kEdge96), middle, row.right - Px(kEdge96), middle + std::max(1, Px(1))};
        ::FillRect(dc, &rule, line.get());
        ::ExcludeClipRect(dc, row.left, row.top, row.right, row.bottom);
        return;
    }

    const bool disabled = (draw.itemState & (ODS_GRAYED | ODS_DISABLED)) != 0;
    const bool selected = (draw.itemState & ODS_SELECTED) != 0;
    // The accent shows through lightly behind the line under the mouse, and on the badges.
    const COLORREF tint = Blend(background, colors_.accent, dark_ ? 22 : 12);
    if (selected) {
        const RECT highlight{row.left + Px(kHighlightInset96), row.top + Px(2), row.right - Px(kHighlightInset96),
                             row.bottom - Px(2)};
        FillRounded(dc, highlight, Px(kHighlightRadius96), tint);
    }
    ::SetBkMode(dc, TRANSPARENT);
    const COLORREF text = disabled ? colors_.mutedText : colors_.text;
    const COLORREF accent = disabled ? colors_.mutedText : colors_.accent;
    const COLORREF quiet = colors_.mutedText;

    int x = row.left + Px(kEdge96);
    const wchar_t glyphChar = GlyphOf(item->glyph);
    if (glyphChar != L'\0') {
        const bool accented = item->glyph == PanelGlyph::Download || selected;
        DrawTextAt(dc, icons_.get(), accented ? accent : quiet, std::wstring_view(&glyphChar, 1),
                   RECT{x, row.top, x + Px(kIcon96), row.bottom}, DT_CENTER);
    }
    x += Px(kIcon96 + kIconGap96);

    const LineText parts = SplitText(*item);
    int right = row.right - Px(kEdge96);
    if (!item->children.empty()) {
        const wchar_t chevron = kChevronRightGlyph;
        DrawTextAt(dc, chevron_.get(), selected ? accent : quiet, std::wstring_view(&chevron, 1),
                   RECT{right - Px(kChevron96), row.top, right, row.bottom}, DT_RIGHT);
        right -= Px(kChevronRoom96);
    }
    if (!parts.size.empty()) {
        const int sizeWidth = TextWidth(dc, text_.get(), parts.size);
        DrawTextAt(dc, text_.get(), quiet, parts.size, RECT{right - sizeWidth, row.top, right, row.bottom}, DT_LEFT);
        right -= sizeWidth + Px(kSizeGap96);
    }

    const int firstWidth = std::min(TextWidth(dc, text_.get(), parts.first), std::max(0, right - x));
    DrawTextAt(dc, text_.get(), text, parts.first, RECT{x, row.top, x + firstWidth, row.bottom}, DT_LEFT | DT_END_ELLIPSIS);
    x += firstWidth;
    if (!parts.detail.empty() && x < right) {
        x += Px(kDetailGap96);
        const int detailWidth = std::min(TextWidth(dc, text_.get(), parts.detail), std::max(0, right - x));
        DrawTextAt(dc, text_.get(), quiet, parts.detail, RECT{x, row.top, x + detailWidth, row.bottom},
                   DT_LEFT | DT_END_ELLIPSIS);
        x += detailWidth;
    }
    if (!item->badge.empty()) {
        x += Px(kBadgeGap96);
        const int badgeWidth = TextWidth(dc, badgeFont_.get(), item->badge) + Px(2 * kBadgePadding96);
        if (x + badgeWidth <= right) {
            const int top = (row.top + row.bottom - Px(kBadgeHeight96)) / 2;
            const RECT chip{x, top, x + badgeWidth, top + Px(kBadgeHeight96)};
            FillRounded(dc, chip, Px(6), selected ? Blend(background, colors_.accent, dark_ ? 40 : 24) : tint);
            DrawTextAt(dc, badgeFont_.get(), accent, item->badge, chip, DT_CENTER);
        }
    }
    // The menu draws its own submenu arrow after this; the chevron above replaces it.
    ::ExcludeClipRect(dc, row.left, row.top, row.right, row.bottom);
}

}  // namespace ldm::ui
