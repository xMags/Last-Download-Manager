#pragma once

#include "ui/Dialog.h"

#include <optional>
#include <string>
#include <vector>

namespace ldm::ui {

// Options > File types > Edit list (checklist O-02): addresses whose downloads the browser keeps.
// Patterns may use '*' as a wildcard.
class AddressExceptionsDialog final : public Dialog {
public:
    AddressExceptionsDialog(const Theme& theme, std::vector<std::wstring> addresses);
    // The edited list; nothing when cancelled.
    [[nodiscard]] std::optional<std::vector<std::wstring>> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;
    bool OnNotify(const NMHDR& header, LRESULT& result) override;
    void Refresh(int select);
    void UpdateButtons();

    std::vector<std::wstring> addresses_;
    std::optional<std::vector<std::wstring>> result_;
};

// One address for the exceptions list.
class AddExceptionDialog final : public Dialog {
public:
    explicit AddExceptionDialog(const Theme& theme);
    [[nodiscard]] std::optional<std::wstring> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;

    std::optional<std::wstring> address_;
};

// A typed address pattern made acceptable for the list: trimmed; nothing when it is empty or
// holds spaces or control characters (patterns are matched against whole addresses).
[[nodiscard]] std::optional<std::wstring> CleanAddressPattern(const std::wstring& text);

}  // namespace ldm::ui
