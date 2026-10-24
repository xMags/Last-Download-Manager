#include "model/Download.h"

#include <array>
#include <utility>

namespace ldm::model {

namespace {

constexpr std::array<std::pair<DownloadStatus, std::string_view>, 8> kStatusNames{{
    {DownloadStatus::Queued, "queued"},
    {DownloadStatus::Downloading, "downloading"},
    {DownloadStatus::Stopping, "stopping"},
    {DownloadStatus::Paused, "paused"},
    {DownloadStatus::Finalizing, "finalizing"},
    {DownloadStatus::Completed, "completed"},
    {DownloadStatus::Failed, "failed"},
    {DownloadStatus::Deleting, "deleting"},
}};

constexpr std::array<std::pair<CompletionAction, std::string_view>, 5> kActionNames{{
    {CompletionAction::None, "none"},
    {CompletionAction::ExitApp, "exit"},
    {CompletionAction::Shutdown, "shutdown"},
    {CompletionAction::Sleep, "sleep"},
    {CompletionAction::Hibernate, "hibernate"},
}};

template <typename Enum, size_t N>
std::string_view NameOf(const std::array<std::pair<Enum, std::string_view>, N>& table, Enum value) noexcept
{
    for (const auto& [candidate, name] : table) {
        if (candidate == value) {
            return name;
        }
    }
    return {};
}

template <typename Enum, size_t N>
std::optional<Enum> ValueOf(const std::array<std::pair<Enum, std::string_view>, N>& table,
                            std::string_view text) noexcept
{
    for (const auto& [value, name] : table) {
        if (name == text) {
            return value;
        }
    }
    return std::nullopt;
}

}  // namespace

std::string_view ToString(DownloadStatus status) noexcept
{
    return NameOf(kStatusNames, status);
}

std::optional<DownloadStatus> ParseDownloadStatus(std::string_view text) noexcept
{
    return ValueOf(kStatusNames, text);
}

std::string_view ToString(CompletionAction action) noexcept
{
    return NameOf(kActionNames, action);
}

std::optional<CompletionAction> ParseCompletionAction(std::string_view text) noexcept
{
    return ValueOf(kActionNames, text);
}

std::int64_t Download::DownloadedBytes() const noexcept
{
    std::int64_t total = 0;
    for (const Segment& segment : segments) {
        total += segment.done;
    }
    for (const MediaTrack& track : tracks) {
        for (const MediaPiece& piece : track.pieces) {
            if (piece.done && !track.wholeFile) {
                total += piece.bytes.value_or(0);
            }
        }
        for (const Segment& range : track.ranges) {
            total += range.done;
        }
    }
    return total;
}

}  // namespace ldm::model
