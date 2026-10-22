#pragma once

#include "core/TransferEngine.h"

#include <string>
#include <string_view>

namespace ldm::core {

// Short user-visible description of a failure reason (checklist ENG-15), for the status column.
[[nodiscard]] std::string_view DescribeFailureReason(FailureReason reason) noexcept;

// The text stored as a download's last result and shown in Properties: description plus detail.
[[nodiscard]] std::string DescribeFailure(const TransferFailure& failure);

}  // namespace ldm::core
