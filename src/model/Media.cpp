#include "model/Media.h"

#include "model/EnumNames.h"

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

}  // namespace

std::string_view ToString(DownloadKind kind) noexcept
{
    return detail::NameOf(kKindNames, kind);
}

std::optional<DownloadKind> ParseDownloadKind(std::string_view text) noexcept
{
    return detail::ValueOf(kKindNames, text);
}

std::string_view ToString(TrackRole role) noexcept
{
    return detail::NameOf(kRoleNames, role);
}

std::optional<TrackRole> ParseTrackRole(std::string_view text) noexcept
{
    return detail::ValueOf(kRoleNames, text);
}

}  // namespace ldm::model
