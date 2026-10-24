#include "model/Queue.h"
#include "model/Schedule.h"

#include "model/EnumNames.h"

#include <array>
#include <utility>

namespace ldm::model {

namespace {

constexpr std::array<std::pair<QueueKind, std::string_view>, 3> kQueueKindNames{{
    {QueueKind::Main, "main"},
    {QueueKind::Sync, "sync"},
    {QueueKind::Custom, "custom"},
}};

constexpr std::array<std::pair<ScheduleMode, std::string_view>, 2> kScheduleModeNames{{
    {ScheduleMode::Once, "once"},
    {ScheduleMode::Daily, "daily"},
}};

}  // namespace

std::string_view ToString(QueueKind kind) noexcept
{
    return detail::NameOf(kQueueKindNames, kind);
}

std::optional<QueueKind> ParseQueueKind(std::string_view text) noexcept
{
    return detail::ValueOf(kQueueKindNames, text);
}

std::string_view ToString(ScheduleMode mode) noexcept
{
    return detail::NameOf(kScheduleModeNames, mode);
}

std::optional<ScheduleMode> ParseScheduleMode(std::string_view text) noexcept
{
    return detail::ValueOf(kScheduleModeNames, text);
}

}  // namespace ldm::model
