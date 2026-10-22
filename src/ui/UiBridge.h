#pragma once

#include "core/Snapshot.h"

#include <windows.h>

#include <memory>
#include <mutex>
#include <vector>

namespace ldm::ui {

// Posted to the main window when a newer snapshot or new notifications are waiting.
inline constexpr UINT kSnapshotMessage = WM_APP + 1;
inline constexpr UINT kNotificationMessage = WM_APP + 2;

// Carries core output to the UI thread (design section 3). The core thread stores the newest
// snapshot and posts at most one message until the UI takes it, so a slow UI sees fewer, newer
// snapshots instead of a backlog. Notifications are queued in order. Thread-safe.
class UiBridge final : public core::CoreObserver {
public:
    // Sets the window to notify; nullptr detaches (call from WM_DESTROY). Pending output is
    // announced to a newly attached window.
    void Attach(HWND window);

    void OnSnapshot(std::shared_ptr<const core::Snapshot> snapshot) override;
    void OnNotification(core::Notification notification) override;

    // UI thread: the newest snapshot (also after it was taken; never null once the core started).
    [[nodiscard]] std::shared_ptr<const core::Snapshot> TakeSnapshot();
    [[nodiscard]] std::vector<core::Notification> TakeNotifications();

private:
    std::mutex mutex_;
    HWND window_ = nullptr;
    std::shared_ptr<const core::Snapshot> latest_;
    bool snapshotPosted_ = false;
    std::vector<core::Notification> notifications_;
    bool notificationPosted_ = false;
};

}  // namespace ldm::ui
