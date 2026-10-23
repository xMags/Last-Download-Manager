#pragma once

#include "model/Category.h"
#include "ui/Handles.h"
#include "ui/ListModel.h"
#include "ui/Theme.h"

#include <windows.h>

#include <functional>
#include <optional>
#include <vector>

namespace ldm::ui {

// The left pane (checklist W-01, W-02, W-04): a "Categories" header with a close button over a tree
// of All Downloads, Unfinished and Finished, each with the categories under it.
class CategoryPane {
public:
    struct Callbacks {
        std::function<void(const ListFilter&)> filterChanged;
        std::function<void()> closeRequested;
        // Right-click on the tree; the category under the cursor, if any, and the screen point.
        std::function<void(std::optional<model::CategoryId>, POINT)> contextMenu;
    };

    CategoryPane() = default;
    CategoryPane(const CategoryPane&) = delete;
    CategoryPane& operator=(const CategoryPane&) = delete;

    void Create(HWND parent, Callbacks callbacks);
    void SetCategories(const std::vector<model::Category>& categories);
    void ApplyTheme(const Theme& theme);
    void SetFont(HFONT font);
    void Layout(const RECT& area);
    void Show(bool visible);

    [[nodiscard]] ListFilter Selected() const;

    bool HandleNotify(const NMHDR& header, LRESULT& result);
    bool HandleCommand(HWND control);
    [[nodiscard]] bool OwnsControl(HWND control) const;
    // WM_CTLCOLORSTATIC for the header label, which sits on the panel color.
    // Returns nothing for controls this pane does not own.
    [[nodiscard]] HBRUSH OnCtlColor(HWND control, HDC dc) const;
    // WM_DRAWITEM for the flat close button; false for other controls.
    bool DrawItem(const DRAWITEMSTRUCT& draw) const;

private:
    void Rebuild();
    void Select(const ListFilter& filter);

    Callbacks callbacks_;
    HWND label_ = nullptr;
    HWND closeButton_ = nullptr;
    HWND tree_ = nullptr;
    UniqueImageList images_;
    UniqueIcon closeIcon_;
    const Theme* theme_ = nullptr;
    std::vector<model::Category> categories_;
    std::vector<ListFilter> nodes_;  // a tree item's lParam is its index here
    bool rebuilding_ = false;
};

}  // namespace ldm::ui
