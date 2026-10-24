#pragma once

#include "model/Download.h"
#include "model/Ids.h"
#include "model/Schedule.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ldm::model {

// The two built-in queues exist from the first start and cannot be deleted or renamed.
enum class QueueKind {
    Main,    // where Download Later puts downloads
    Sync,    // checks its completed members for changes on the server (checklist Q-05)
    Custom,  // made by the user
};

[[nodiscard]] std::string_view ToString(QueueKind kind) noexcept;
[[nodiscard]] std::optional<QueueKind> ParseQueueKind(std::string_view text) noexcept;

inline constexpr int kMaxQueueConcurrent = 16;
inline constexpr int kMaxQueueRetries = 99;

// A download queue (checklist Q-01 to Q-08). Its members are the downloads whose `queueId` names
// it. Whether it runs is not stored: every queue is stopped when LDM starts.
struct Queue {
    QueueId id;
    std::string name;
    QueueKind kind = QueueKind::Custom;
    int concurrent = 1;      // members downloading at the same time, 1 to kMaxQueueConcurrent
    int retriesPerFile = 3;  // failed attempts of a member in one run, 0 to kMaxQueueRetries
    CompletionAction completionAction = CompletionAction::None;
    bool forceTerminate = false;      // shut down even when programs refuse to close
    std::string openFileWhenDone;     // a program or document to open when the queue finishes; empty for none
    Schedule schedule;
    std::int64_t sortOrder = 0;

    [[nodiscard]] bool IsBuiltin() const noexcept { return kind != QueueKind::Custom; }
};

}  // namespace ldm::model
