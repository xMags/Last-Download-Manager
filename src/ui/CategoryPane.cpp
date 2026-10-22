#include "ui/CategoryPane.h"

#include "base/Utf.h"
#include "ui/Dpi.h"
#include "ui/GlyphIcons.h"

#include <commctrl.h>
#include <windowsx.h>

#include <array>

namespace ldm::ui {

namespace {

enum Image {
    kImageAll,
    kImageFolder,
    kImageUnfinished,
    kImageFinished,
};

constexpr std::array<Glyph, 4> kTreeGlyphs{{
    {0xE896, RGB(0x1E, 0x88, 0xE5)},
    {0xE8B7, RGB(0xE8, 0xA3, 0x17)},
    {0xE823, RGB(0xF0, 0x8C, 0x00)},
    {0xE930, RGB(0x2E, 0xA0, 0x43)},
}};

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
    closeButton_ = ::CreateWindowExW(0, WC_BUTTONW, L"✕", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT, 0, 0, 0, 0,
                                     parent, nullptr, instance, nullptr);
    tree_ = ::CreateWindowExW(0, WC_TREEVIEWW, nullptr,
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_SHOWSELALWAYS |
                                  TVS_FULLROWSELECT | TVS_TRACKSELECT,
                              0, 0, 0, 0, parent, nullptr, instance, nullptr);
    TreeView_SetExtendedStyle(tree_, TVS_EX_DOUBLEBUFFER | TVS_EX_FADEINOUTEXPANDOS, TVS_EX_DOUBLEBUFFER | TVS_EX_FADEINOUTEXPANDOS);
    images_ = CreateGlyphImageList(kTreeGlyphs, Scale(16, DpiOf(tree_)));
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
            insert(parent, base::ToUtf16(category.name), kImageFolder, ListFilter{root.status, category.id});
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
    theme.ApplyToControl(tree_);
    theme.ApplyToControl(closeButton_);
    TreeView_SetBkColor(tree_, theme.Colors().window);
    TreeView_SetTextColor(tree_, theme.Colors().text);
    images_ = CreateGlyphImageList(kTreeGlyphs, Scale(16, DpiOf(tree_)));
    TreeView_SetImageList(tree_, images_.get(), TVSIL_NORMAL);
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
    const int headerHeight = Scale(24, dpi);
    const int buttonSize = Scale(20, dpi);
    const int margin = Scale(4, dpi);
    const int width = area.right - area.left;
    ::MoveWindow(label_, area.left + margin, area.top, width - buttonSize - 2 * margin, headerHeight, TRUE);
    ::MoveWindow(closeButton_, area.right - buttonSize - margin / 2, area.top + (headerHeight - buttonSize) / 2, buttonSize,
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
