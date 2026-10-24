#include "ui/CategoryPane.h"

#include "base/Utf.h"
#include "ui/Dpi.h"
#include "ui/GlyphIcons.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <string_view>

namespace ldm::ui {

namespace {

enum Image {
    kImageAll,
    kImageFolder,
    kImageUnfinished,
    kImageFinished,
    kImageCompressed,
    kImageDocuments,
    kImageMusic,
    kImagePrograms,
    kImageVideo,
};

constexpr wchar_t kCloseGlyph = 0xE8BB;  // ChromeClose
constexpr int kHeaderHeight96 = 22;
constexpr int kCloseButton96 = 18;
constexpr int kCloseGlyph96 = 10;
constexpr int kTreeItemHeight96 = 21;
constexpr int kTreeIcon96 = 14;  // tree icons sit inside short rows with room to spare

// In Image order. Each built-in category has its own icon, drawn from the Windows icon font.
constexpr std::array<Glyph, 9> kTreeGlyphs{{
    {0xE838, RGB(0xE8, 0xA3, 0x17)},  // FolderOpen
    {0xE8B7, RGB(0xE8, 0xA3, 0x17)},  // Folder
    {0xE896, RGB(0x2E, 0xA0, 0x43)},  // Download
    {0xE930, RGB(0x1E, 0x88, 0xE5)},  // Completed
    {0xF012, RGB(0xC9, 0x8A, 0x10)},  // ZipFolder
    {0xE8A5, RGB(0x60, 0x7D, 0x8B)},  // Document
    {0xEC4F, RGB(0x1E, 0x88, 0xE5)},  // MusicNote
    {0xE737, RGB(0x45, 0x5A, 0x64)},  // Favicon (a window)
    {0xE714, RGB(0x26, 0x8C, 0x7A)},  // Video
}};

// Built-in categories are seeded with these names; renamed or user categories get a folder.
int CategoryImage(const model::Category& category)
{
    if (!category.builtin) {
        return kImageFolder;
    }
    const struct {
        std::string_view name;
        Image image;
    } known[] = {{"Compressed", kImageCompressed}, {"Documents", kImageDocuments}, {"Music", kImageMusic},
                 {"Programs", kImagePrograms},     {"Video", kImageVideo}};
    for (const auto& entry : known) {
        if (category.name == entry.name) {
            return entry.image;
        }
    }
    return kImageFolder;
}

bool SameCategories(const std::vector<model::Category>& a, const std::vector<model::Category>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].name != b[i].name) {
            return false;
        }
    }
    return true;
}

}  // namespace

void CategoryPane::Create(HWND parent, Callbacks callbacks)
{
    callbacks_ = std::move(callbacks);
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    label_ = ::CreateWindowExW(0, WC_STATICW, L"Categories", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, 0, 0, 0, 0,
                               parent, nullptr, instance, nullptr);
    closeButton_ = ::CreateWindowExW(0, WC_BUTTONW, L"Hide categories", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0,
                                     parent, nullptr, instance, nullptr);
    // A classic tree: dotted lines and boxed expand buttons on every level, roots included.
    tree_ = ::CreateWindowExW(0, WC_TREEVIEWW, nullptr,
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
                                  TVS_SHOWSELALWAYS,
                              0, 0, 0, 0, parent, nullptr, instance, nullptr);
    TreeView_SetExtendedStyle(tree_, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER | TVS_EX_FADEINOUTEXPANDOS);
    images_ = CreateGlyphImageList(kTreeGlyphs, Scale(kTreeIcon96, DpiOf(tree_)));
    TreeView_SetImageList(tree_, images_.get(), TVSIL_NORMAL);
}

void CategoryPane::SetCategories(const std::vector<model::Category>& categories)
{
    if (SameCategories(categories, categories_) && !nodes_.empty()) {
        categories_ = categories;
        return;
    }
    categories_ = categories;
    Rebuild();
}

void CategoryPane::Rebuild()
{
    const ListFilter selected = nodes_.empty() ? ListFilter{} : Selected();
    rebuilding_ = true;
    TreeView_DeleteAllItems(tree_);
    nodes_.clear();

    const auto insert = [this](HTREEITEM parent, const std::wstring& text, int image, const ListFilter& filter) {
        nodes_.push_back(filter);
        TVINSERTSTRUCTW item{};
        item.hParent = parent;
        item.hInsertAfter = TVI_LAST;
        item.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM;
        item.item.pszText = const_cast<wchar_t*>(text.c_str());  // the tree copies the text
        item.item.iImage = image;
        item.item.iSelectedImage = image;
        item.item.lParam = static_cast<LPARAM>(nodes_.size() - 1);
        return TreeView_InsertItem(tree_, &item);
    };
    const struct {
        const wchar_t* title;
        int image;
        StatusFilter status;
    } roots[] = {
        {L"All Downloads", kImageAll, StatusFilter::All},
        {L"Unfinished", kImageUnfinished, StatusFilter::Unfinished},
        {L"Finished", kImageFinished, StatusFilter::Finished},
    };
    for (const auto& root : roots) {
        const HTREEITEM parent = insert(TVI_ROOT, root.title, root.image, ListFilter{root.status, std::nullopt});
        for (const model::Category& category : categories_) {
            insert(parent, base::ToUtf16(category.name), CategoryImage(category), ListFilter{root.status, category.id});
        }
        if (root.status == StatusFilter::All) {
            TreeView_Expand(tree_, parent, TVE_EXPAND);
        }
    }
    Select(selected);
    rebuilding_ = false;
}

void CategoryPane::Select(const ListFilter& filter)
{
    for (HTREEITEM root = TreeView_GetRoot(tree_); root != nullptr; root = TreeView_GetNextSibling(tree_, root)) {
        for (HTREEITEM item = root; item != nullptr;
             item = item == root ? TreeView_GetChild(tree_, root) : TreeView_GetNextSibling(tree_, item)) {
            TVITEMW data{};
            data.mask = TVIF_PARAM;
            data.hItem = item;
            TreeView_GetItem(tree_, &data);
            if (nodes_[static_cast<size_t>(data.lParam)] == filter) {
                TreeView_SelectItem(tree_, item);
                return;
            }
        }
    }
    TreeView_SelectItem(tree_, TreeView_GetRoot(tree_));
}

ListFilter CategoryPane::Selected() const
{
    const HTREEITEM item = TreeView_GetSelection(tree_);
    if (item == nullptr) {
        return {};
    }
    TVITEMW data{};
    data.mask = TVIF_PARAM;
    data.hItem = item;
    TreeView_GetItem(tree_, &data);
    const auto index = static_cast<size_t>(data.lParam);
    return index < nodes_.size() ? nodes_[index] : ListFilter{};
}

void CategoryPane::ApplyTheme(const Theme& theme)
{
    theme_ = &theme;
    const UINT dpi = DpiOf(tree_);
    theme.ApplyToControl(tree_);
    if (!theme.IsDark()) {
        // The plain tree theme draws dotted lines and boxed buttons; Explorer's hides them.
        ::SetWindowTheme(tree_, nullptr, nullptr);
    }
    TreeView_SetBkColor(tree_, theme.Colors().window);
    TreeView_SetTextColor(tree_, theme.Colors().text);
    images_ = CreateGlyphImageList(kTreeGlyphs, Scale(kTreeIcon96, dpi));
    TreeView_SetImageList(tree_, images_.get(), TVSIL_NORMAL);
    TreeView_SetItemHeight(tree_, Scale(kTreeItemHeight96, dpi));
    closeIcon_ = CreateGlyphIcon(Glyph{kCloseGlyph, theme.Colors().text}, Scale(kCloseGlyph96, dpi));
    ::InvalidateRect(label_, nullptr, TRUE);
    ::InvalidateRect(closeButton_, nullptr, TRUE);
}

HBRUSH CategoryPane::OnCtlColor(HWND control, HDC dc) const
{
    if (control != label_ || theme_ == nullptr) {
        return nullptr;
    }
    ::SetTextColor(dc, theme_->Colors().text);
    ::SetBkColor(dc, theme_->Colors().panel);
    return theme_->PanelBrush();
}

bool CategoryPane::DrawItem(const DRAWITEMSTRUCT& draw) const
{
    if (draw.hwndItem != closeButton_ || theme_ == nullptr) {
        return false;
    }
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    UniqueBrush pressedBrush(::CreateSolidBrush(theme_->Colors().divider));
    ::FillRect(draw.hDC, &draw.rcItem, pressed ? pressedBrush.get() : theme_->PanelBrush());
    const int glyph = Scale(kCloseGlyph96, DpiOf(closeButton_));
    const int x = (draw.rcItem.left + draw.rcItem.right - glyph) / 2;
    const int y = (draw.rcItem.top + draw.rcItem.bottom - glyph) / 2;
    ::DrawIconEx(draw.hDC, x, y, closeIcon_.get(), glyph, glyph, 0, nullptr, DI_NORMAL);
    if ((draw.itemState & ODS_FOCUS) != 0 && (draw.itemState & ODS_NOFOCUSRECT) == 0) {
        RECT focus = draw.rcItem;
        ::InflateRect(&focus, -2, -2);
        ::DrawFocusRect(draw.hDC, &focus);
    }
    return true;
}

void CategoryPane::SetFont(HFONT font)
{
    for (HWND control : {label_, closeButton_, tree_}) {
        ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void CategoryPane::Layout(const RECT& area)
{
    const UINT dpi = DpiOf(tree_);
    const int headerHeight = Scale(kHeaderHeight96, dpi);
    const int buttonSize = Scale(kCloseButton96, dpi);
    const int margin = Scale(4, dpi);
    const int width = area.right - area.left;
    ::MoveWindow(label_, area.left + margin, area.top, std::max(0, width - buttonSize - 2 * margin), headerHeight, TRUE);
    ::MoveWindow(closeButton_, area.right - buttonSize - Scale(2, dpi), area.top + (headerHeight - buttonSize) / 2, buttonSize,
                 buttonSize, TRUE);
    ::MoveWindow(tree_, area.left, area.top + headerHeight, width, area.bottom - area.top - headerHeight, TRUE);
}

void CategoryPane::Show(bool visible)
{
    for (HWND control : {label_, closeButton_, tree_}) {
        ::ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
    }
}

bool CategoryPane::OwnsControl(HWND control) const
{
    return control == label_ || control == closeButton_ || control == tree_;
}

bool CategoryPane::HandleCommand(HWND control)
{
    if (control == closeButton_) {
        if (callbacks_.closeRequested) {
            callbacks_.closeRequested();
        }
        return true;
    }
    return false;
}

bool CategoryPane::HandleNotify(const NMHDR& header, LRESULT& result)
{
    if (header.hwndFrom != tree_) {
        return false;
    }
    switch (header.code) {
    case TVN_SELCHANGEDW:
        if (!rebuilding_ && callbacks_.filterChanged) {
            callbacks_.filterChanged(Selected());
        }
        result = 0;
        return true;
    case NM_RCLICK: {
        POINT screen{};
        ::GetCursorPos(&screen);
        TVHITTESTINFO hit{};
        hit.pt = screen;
        ::ScreenToClient(tree_, &hit.pt);
        std::optional<model::CategoryId> category;
        if (const HTREEITEM item = TreeView_HitTest(tree_, &hit); item != nullptr) {
            TreeView_SelectItem(tree_, item);
            category = Selected().category;
        }
        if (callbacks_.contextMenu) {
            callbacks_.contextMenu(category, screen);
        }
        result = TRUE;
        return true;
    }
    default:
        return false;
    }
}

}  // namespace ldm::ui
