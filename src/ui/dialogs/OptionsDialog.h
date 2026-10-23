#pragma once

#include "browser/Protocol.h"
#include "core/Commands.h"
#include "core/Ports.h"
#include "core/Snapshot.h"
#include "ui/Dialog.h"
#include "ui/SystemActions.h"
#include "ui/dialogs/CaptureDialogs.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace ldm::ui {

// Downloads > Options (checklist O-01 to O-09). One tab per page; each page is a child dialog
// holding controls whose ids are unique across all pages. Works on copies; nothing is applied
// until OK.
class OptionsDialog final : public Dialog {
public:
    struct Input {
        model::Settings settings;
        std::vector<model::Category> categories;
        std::vector<model::ServerLimit> serverLimits;
        std::vector<core::SiteLoginView> siteLogins;
        bool launchOnStartup = false;
        std::vector<browser::Hello> connectedBrowsers;  // shown on General; read only
    };

    struct Output {
        model::Settings settings;
        std::vector<model::Category> changedCategories;
        std::optional<std::vector<model::ServerLimit>> serverLimits;  // set when edited
        std::optional<std::vector<core::SiteLoginInput>> siteLogins;  // set when edited
        bool launchOnStartup = false;
    };

    OptionsDialog(const Theme& theme, Input input, core::SecretProtector& secrets, SystemActions& system);
    [[nodiscard]] std::optional<Output> Run(HWND owner);

    // Copies the settings this dialog edits from `edited` into `current`, leaving everything else
    // (window layout, history, the limiter state) as the core has it now.
    static void ApplyEditedFields(const model::Settings& edited, model::Settings& current);

private:
    static constexpr int kPageCount = 8;

    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;
    bool OnNotify(const NMHDR& header, LRESULT& result) override;
    void OnDestroying() override;

    // Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+PgDn and Ctrl+PgUp switch pages, as in Windows property
    // sheets. A modal dialog's keys never reach the app's message loop, so a message filter hook
    // installed for the dialog's lifetime catches them.
    static LRESULT CALLBACK PageKeyFilter(int code, WPARAM wParam, LPARAM lParam);
    void StepPage(int delta);

    static INT_PTR CALLBACK PageProc(HWND page, UINT message, WPARAM wParam, LPARAM lParam);
    void OnPageCommand(int id, int code);
    void OnPageNotify(const NMHDR& header);
    void ShowPage(int index);

    [[nodiscard]] HWND Control(int id) const;
    [[nodiscard]] std::wstring Get(int id) const;
    void Put(int id, const std::wstring& text);
    [[nodiscard]] bool IsChecked(int id) const;
    void Check(int id, bool checked);
    [[nodiscard]] std::optional<int> GetNumber(int id) const;

    void Load();
    bool Store();
    void LoadCategory(int index);
    void StoreCategory(int index);
    void LoadProxy(int enabledId, int hostId, int portId, int userId, const model::ProxyServer& server);
    bool StoreProxy(int enabledId, int hostId, int portId, int userId, int passwordId, model::ProxyServer& server);
    void ShowConnectedBrowsers();
    void ShowAddressExceptionCount();
    void RefreshLimits();
    void RefreshLogins();
    void UpdateProxyControls();
    void UpdateStartDialogControls();
    void UpdateQuotaControls();

    Input input_;
    std::vector<std::wstring> addressExceptions_;  // edited in their own dialog
    model::CaptureKeys captureKeys_;               // edited in their own dialog
    BrowserMenus browserMenus_;                    // likewise
    HHOOK pageKeyHook_ = nullptr;
    core::SecretProtector& secrets_;
    SystemActions& system_;
    std::array<HWND, kPageCount> pages_{};
    std::vector<model::Category> categories_;
    int currentCategory_ = -1;
    std::vector<model::ServerLimit> limits_;
    bool limitsEdited_ = false;
    std::vector<core::SiteLoginInput> logins_;
    bool loginsEdited_ = false;
    std::optional<Output> result_;
};

}  // namespace ldm::ui
