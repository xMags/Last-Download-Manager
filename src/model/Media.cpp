#include "model/Media.h"

#include <array>
#include <utility>

namespace ldm::model {

namespace {

constexpr std::array<std::pair<DownloadKind, std::string_view>, 3> kKindNames{{
    {DownloadKind::File, "file"},
    {DownloadKind::Hls, "hls"},
    {DownloadKind::Dash, "dash"},
}};

constexpr std::array<std::pair<TrackRole, std::string_view>, 3> kRoleNames{{
    {TrackRole::Video, "video"},
    {TrackRole::Audio, "audio"},
    {TrackRole::Subtitles, "subtitles"},
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

std::string_view ToString(DownloadKind kind) noexcept
{
    return NameOf(kKindNames, kind);
}

std::optional<DownloadKind> ParseDownloadKind(std::string_view text) noexcept
{
    return ValueOf(kKindNames, text);
}

std::string_view ToString(TrackRole role) noexcept
{
    return NameOf(kRoleNames, role);
}

std::optional<TrackRole> ParseTrackRole(std::string_view text) noexcept
{
    return ValueOf(kRoleNames, text);
}

}  // namespace ldm::model
