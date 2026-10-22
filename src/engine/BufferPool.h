#pragma once

#include <cstdint>
#include <functional>
#include <mutex>

namespace ldm::engine {

// Byte budget for received data waiting to be written (design section 3). The network thread
// reserves before keeping a chunk; the disk thread releases after writing it. When a reservation
// fails, the next release calls `onAvailable` once, so the network thread can resume paused
// connections. Thread-safe.
class BufferPool {
public:
    BufferPool(std::int64_t capacityBytes, std::function<void()> onAvailable);

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    [[nodiscard]] bool TryReserve(std::int64_t bytes);
    void Release(std::int64_t bytes);

    [[nodiscard]] std::int64_t InUse() const;

private:
    const std::int64_t capacity_;
    const std::function<void()> onAvailable_;

    mutable std::mutex mutex_;
    std::int64_t inUse_ = 0;
    bool starved_ = false;
};

}  // namespace ldm::engine
