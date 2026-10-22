#include "engine/BufferPool.h"

#include <cassert>
#include <utility>

namespace ldm::engine {

BufferPool::BufferPool(std::int64_t capacityBytes, std::function<void()> onAvailable)
    : capacity_(capacityBytes), onAvailable_(std::move(onAvailable))
{
}

bool BufferPool::TryReserve(std::int64_t bytes)
{
    std::lock_guard lock(mutex_);
    // A single chunk larger than the whole pool is still admitted when nothing else is held, so
    // a transfer can never deadlock on an oversized chunk.
    if (inUse_ > 0 && inUse_ + bytes > capacity_) {
        starved_ = true;
        return false;
    }
    inUse_ += bytes;
    return true;
}

void BufferPool::Release(std::int64_t bytes)
{
    bool notify = false;
    {
        std::lock_guard lock(mutex_);
        assert(bytes <= inUse_);
        inUse_ -= bytes;
        notify = std::exchange(starved_, false);
    }
    // Called without the lock held: the callback reaches into the network thread's wakeup.
    if (notify && onAvailable_) {
        onAvailable_();
    }
}

std::int64_t BufferPool::InUse() const
{
    std::lock_guard lock(mutex_);
    return inUse_;
}

}  // namespace ldm::engine
