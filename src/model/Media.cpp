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

constexpr std::array<std::pair<SiteQuality, std::string_view>, 7> kSiteQualityNames{{
    {SiteQuality::Best, "best"},
    {SiteQuality::UpTo2160, "2160"},
    {SiteQuality::UpTo1440, "1440"},
    {SiteQuality::UpTo1080, "1080"},
    {SiteQuality::UpTo720, "720"},
    {SiteQuality::UpTo480, "480"},
    {SiteQuality::AudioOnly, "audio"},
}};

}  // namespace

std::string_view ToString(SiteQuality quality) noexcept
{
    return detail::NameOf(kSiteQualityNames, quality);
}

std::optional<SiteQuality> ParseSiteQuality(std::string_view text) noexcept
{
    return detail::ValueOf(kSiteQualityNames, text);
}

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
