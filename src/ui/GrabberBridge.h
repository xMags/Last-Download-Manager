#pragma once

#include "grabber/GrabberSnapshot.h"

#include <windows.h>

#include <memory>
#include <mutex>
#include <vector>

namespace ldm::ui {

// Posted to the main window when a newer Grabber snapshot or Grabber notifications are waiting.
inline constexpr UINT kGrabberSnapshotMessage = WM_APP + 4;
inline constexpr UINT kGrabberNotificationMessage = WM_APP + 5;

// Carries the Grabber explorer's output to the UI thread, as UiBridge does for the coordinator's:
// the newest snapshot with at most one message waiting, and notifications in order. Thread-safe.
class GrabberBridge final : public grabber::GrabberObserver {
public:
    // Sets the window to notify; nullptr detaches (call from WM_DESTROY).
    void Attach(HWND window);

    void OnGrabberSnapshot(std::shared_ptr<const grabber::GrabberSnapshot> snapshot) override;
    void OnGrabberNotification(grabber::GrabberNotification notification) override;

    // UI thread.
    [[nodiscard]] std::shared_ptr<const grabber::GrabberSnapshot> TakeSnapshot();
    [[nodiscard]] std::vector<grabber::GrabberNotification> TakeNotifications();

private:
    std::mutex mutex_;
    HWND window_ = nullptr;
    std::shared_ptr<const grabber::GrabberSnapshot> latest_;
    bool snapshotPosted_ = false;
    std::vector<grabber::GrabberNotification> notifications_;
    bool notificationPosted_ = false;
};

}  // namespace ldm::ui
