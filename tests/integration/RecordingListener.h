#pragma once

#include "core/TransferEngine.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace ldm::test {

// Keeps every engine event, for tests to wait on.
class RecordingListener final : public ldm::core::EngineListener {
public:
    void OnEngineEvent(ldm::core::EngineEvent event) override
    {
        {
            std::lock_guard lock(mutex_);
            events_.push_back(std::move(event));
        }
        changed_.notify_all();
    }

    // Waits for an event of type T for (id, epoch) matching `predicate`.
    template <typename T>
    std::optional<T> WaitFor(ldm::model::DownloadId id, std::int64_t epoch, std::chrono::milliseconds timeout,
                             const std::function<bool(const T&)>& predicate = {})
    {
        std::unique_lock lock(mutex_);
        std::optional<T> found;
        changed_.wait_for(lock, timeout, [&] {
            for (const ldm::core::EngineEvent& event : events_) {
                if (event.id != id || event.epoch != epoch) {
                    continue;
                }
                if (const T* typed = std::get_if<T>(&event.payload); typed && (!predicate || predicate(*typed))) {
                    found = *typed;
                    return true;
                }
            }
            return false;
        });
        return found;
    }

    std::optional<ldm::core::TransferFinished> Finished(ldm::model::DownloadId id, std::int64_t epoch, std::chrono::milliseconds timeout = std::chrono::seconds(60))
    {
        return WaitFor<ldm::core::TransferFinished>(id, epoch, timeout);
    }

    template <typename T>
    std::vector<T> All(ldm::model::DownloadId id, std::int64_t epoch)
    {
        std::lock_guard lock(mutex_);
        std::vector<T> result;
        for (const ldm::core::EngineEvent& event : events_) {
            if (event.id == id && event.epoch == epoch) {
                if (const T* typed = std::get_if<T>(&event.payload)) {
                    result.push_back(*typed);
                }
            }
        }
        return result;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<ldm::core::EngineEvent> events_;
};

}  // namespace ldm::test
