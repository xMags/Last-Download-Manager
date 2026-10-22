#pragma once

#include "ui/Handles.h"
#include "ui/Theme.h"

#include <windows.h>

#include <string>
#include <vector>

namespace ldm::ui {

// The main toolbar (checklist TB-01, TB-02): glyph icons, labels under large buttons, tooltips,
// the standard Customize dialog, and a stored button order.
class Toolbar {
public:
    Toolbar() = default;
    Toolbar(const Toolbar&) = delete;
    Toolbar& operator=(const Toolbar&) = delete;

    void Create(HWND parent, int controlId, bool largeButtons, const std::string& layout, const Theme& theme);
    [[nodiscard]] HWND Handle() const noexcept { return window_; }
    [[nodiscard]] int PreferredHeight() const;

    // Rebuilds icons and metrics for a new DPI, theme or button size.
    void Refresh(bool largeButtons, const Theme& theme);
    void SetEnabled(int commandId, bool enabled);
    void Customize();

    // The current button order, in the form `Create` accepts ("cmd,cmd,0,cmd": 0 is a separator).
    [[nodiscard]] std::string Layout() const;

    // WM_NOTIFY from the toolbar. Returns true when handled; `result` is the message result.
    // `layoutChanged` is set when the user changed the buttons in the Customize dialog.
    bool HandleNotify(const NMHDR& header, LRESULT& result, bool& layoutChanged);

private:
    void ApplyLayout(const std::string& layout);
    void ApplyImages(const Theme& theme);

    HWND window_ = nullptr;
    bool large_ = true;
    UniqueImageList images_;
    UniqueImageList disabledImages_;
    std::vector<int> savedOrder_;  // restored by the Customize dialog's Reset button
    COLORREF textColor_ = 0;
    COLORREF background_ = 0;
};

}  // namespace ldm::ui
