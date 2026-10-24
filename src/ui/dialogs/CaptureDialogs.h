#pragma once

#include "model/Settings.h"
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

// Options > General > Keys (checklist B-03): keys that keep a clicked link in the browser or give
// it to LDM.
class CaptureKeysDialog final : public Dialog {
public:
    CaptureKeysDialog(const Theme& theme, model::CaptureKeys keys);
    [[nodiscard]] std::optional<model::CaptureKeys> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;
    void UpdateControls();

    model::CaptureKeys keys_;
    std::optional<model::CaptureKeys> result_;
};

// The browser right-click menu items, per browser family (checklist O-01).
struct BrowserMenus {
    model::BrowserMenu chromium;
    model::BrowserMenu firefox;
};

// Options > General > Context menu.
class BrowserMenuDialog final : public Dialog {
public:
    BrowserMenuDialog(const Theme& theme, BrowserMenus menus);
    [[nodiscard]] std::optional<BrowserMenus> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;

    BrowserMenus menus_;
    std::optional<BrowserMenus> result_;
};

// The media panel's settings (checklists B-07, B-09).
struct MediaPanelSettings {
    bool enabled = true;
    std::string types;           // space separated
    int minimumKilobytes = 0;
    std::string siteExceptions;  // space separated
    bool drmNotice = true;
};

// Options > General > Media panel.
class MediaPanelDialog final : public Dialog {
public:
    MediaPanelDialog(const Theme& theme, MediaPanelSettings settings);
    [[nodiscard]] std::optional<MediaPanelSettings> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;

    MediaPanelSettings settings_;
    std::optional<MediaPanelSettings> result_;
};

// Offered after two captured downloads from one site were cancelled (checklist B-06): add the site
// or an address pattern to the capture exceptions.
class ExceptionOfferDialog final : public Dialog {
public:
    enum class Kind {
        Site,
        Address,
    };

    struct Choice {
        std::optional<Kind> kind;  // nothing: Cancel
        std::wstring value;        // the site or the address pattern, as edited
        bool dontAskAgain = false;  // also after Cancel
    };

    ExceptionOfferDialog(const Theme& theme, std::wstring site, std::wstring address, Kind preselected);
    [[nodiscard]] Choice Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;
    void UpdateFields();

    std::wstring site_;
    std::wstring address_;
    Kind preselected_;
    Choice choice_;
};

// A typed address pattern made acceptable for the list: trimmed; nothing when it is empty or
// holds spaces or control characters (patterns are matched against whole addresses).
[[nodiscard]] std::optional<std::wstring> CleanAddressPattern(const std::wstring& text);

}  // namespace ldm::ui
