#include "ui/GrabberBridge.h"

#include <utility>

namespace ldm::ui {

void GrabberBridge::Attach(HWND window)
{
    std::lock_guard lock(mutex_);
    window_ = window;
    snapshotPosted_ = false;
    notificationPosted_ = false;
    if (window_ == nullptr) {
        return;
    }
    if (latest_) {
        snapshotPosted_ = ::PostMessageW(window_, kGrabberSnapshotMessage, 0, 0) != FALSE;
    }
    if (!notifications_.empty()) {
        notificationPosted_ = ::PostMessageW(window_, kGrabberNotificationMessage, 0, 0) != FALSE;
    }
}

void GrabberBridge::OnGrabberSnapshot(std::shared_ptr<const grabber::GrabberSnapshot> snapshot)
{
    std::lock_guard lock(mutex_);
    latest_ = std::move(snapshot);
    if (window_ != nullptr && !snapshotPosted_) {
        snapshotPosted_ = ::PostMessageW(window_, kGrabberSnapshotMessage, 0, 0) != FALSE;
    }
}

void GrabberBridge::OnGrabberNotification(grabber::GrabberNotification notification)
{
    std::lock_guard lock(mutex_);
    notifications_.push_back(std::move(notification));
    if (window_ != nullptr && !notificationPosted_) {
        notificationPosted_ = ::PostMessageW(window_, kGrabberNotificationMessage, 0, 0) != FALSE;
    }
}

std::shared_ptr<const grabber::GrabberSnapshot> GrabberBridge::TakeSnapshot()
{
    std::lock_guard lock(mutex_);
    snapshotPosted_ = false;
    return latest_;
}

std::vector<grabber::GrabberNotification> GrabberBridge::TakeNotifications()
{
    std::lock_guard lock(mutex_);
    notificationPosted_ = false;
    return std::exchange(notifications_, {});
}

}  // namespace ldm::ui
