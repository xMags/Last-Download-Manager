#pragma once

#include "core/Commands.h"
#include "core/Snapshot.h"
#include "ui/Dialog.h"

#include <functional>
#include <memory>

namespace ldm::ui {

// The per-download progress window (checklist P-01 to P-06): status, speed limiter and
// completion tabs, a bar showing each connection's part of the file, and the connection table.
// Modeless; it lives until the user closes it or the download leaves the running states.
class ProgressWindow final : public Dialog {
public:
    struct Callbacks {
        std::function<void(core::Command)> post;
        // Called after the window was destroyed; the owner deletes the object.
        std::function<void(model::DownloadId)> closed;
    };

    ProgressWindow(const Theme& theme, model::DownloadId id, Callbacks callbacks);

    // Creates the window according to the "start view" setting (checklist P-06).
    void Open(HWND owner, const core::DownloadView& view, const model::Settings& settings);
    void Update(const core::DownloadView& view, const model::Settings& settings);
    [[nodiscard]] model::DownloadId Id() const noexcept { return id_; }

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;
    bool OnNotify(const NMHDR& header, LRESULT& result) override;
    bool OnMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) override;
    void OnDestroyed() override;

    void ShowPage(int page);
    void SetDetailsVisible(bool visible);
    void ApplySpeedLimit();
    // Stop, asking first for a live recording. False when the user cancelled.
    bool StopThis();
    void DrawSegments(const DRAWITEMSTRUCT& item) const;
    void UpdateConnections(const core::DownloadView& view);

    model::DownloadId id_;
    Callbacks callbacks_;
    std::shared_ptr<const core::DownloadView> latest_;
    std::vector<int> pageForTab_;  // tab index -> page number (0 status, 1 speed, 2 completion)
    bool detailsVisible_ = true;
    int fullHeight_ = 0;
    int collapsedHeight_ = 0;
    int heightsDpi_ = 0;  // the scale fullHeight_ and collapsedHeight_ are measured at
    bool updating_ = false;
    bool rememberLimit_ = true;
};

}  // namespace ldm::ui
