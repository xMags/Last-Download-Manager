#include "ui/UiBridge.h"

#include <utility>

namespace ldm::ui {

void UiBridge::Attach(HWND window)
{
    std::lock_guard lock(mutex_);
    window_ = window;
    snapshotPosted_ = false;
    notificationPosted_ = false;
    if (window_ == nullptr) {
        return;
    }
    if (latest_) {
        snapshotPosted_ = ::PostMessageW(window_, kSnapshotMessage, 0, 0) != FALSE;
    }
    if (!notifications_.empty()) {
        notificationPosted_ = ::PostMessageW(window_, kNotificationMessage, 0, 0) != FALSE;
    }
}

void UiBridge::OnSnapshot(std::shared_ptr<const core::Snapshot> snapshot)
{
    std::lock_guard lock(mutex_);
    latest_ = std::move(snapshot);
    // PostMessage only queues; holding the lock here cannot deadlock with the UI thread.
    if (window_ != nullptr && !snapshotPosted_) {
        snapshotPosted_ = ::PostMessageW(window_, kSnapshotMessage, 0, 0) != FALSE;
    }
}

void UiBridge::OnNotification(core::Notification notification)
{
    std::lock_guard lock(mutex_);
    notifications_.push_back(std::move(notification));
    if (window_ != nullptr && !notificationPosted_) {
        notificationPosted_ = ::PostMessageW(window_, kNotificationMessage, 0, 0) != FALSE;
    }
}

std::shared_ptr<const core::Snapshot> UiBridge::TakeSnapshot()
{
    std::lock_guard lock(mutex_);
    snapshotPosted_ = false;
    return latest_;
}

std::vector<core::Notification> UiBridge::TakeNotifications()
{
    std::lock_guard lock(mutex_);
    notificationPosted_ = false;
    return std::exchange(notifications_, {});
}

}  // namespace ldm::ui
