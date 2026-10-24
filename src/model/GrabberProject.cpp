#include "model/GrabberProject.h"

#include "model/EnumNames.h"

#include <array>
#include <utility>

namespace ldm::model {

namespace {

constexpr std::array<std::pair<GrabberTemplate, std::string_view>, 7> kTemplateNames{{
    {GrabberTemplate::Custom, "custom"},
    {GrabberTemplate::Pictures, "pictures"},
    {GrabberTemplate::Audio, "audio"},
    {GrabberTemplate::Video, "video"},
    {GrabberTemplate::AllFiles, "all_files"},
    {GrabberTemplate::WholeSite, "whole_site"},
    {GrabberTemplate::StartPageOffline, "start_page_offline"},
}};

constexpr std::array<std::pair<GrabberSaveMode, std::string_view>, 3> kSaveModeNames{{
    {GrabberSaveMode::ByCategory, "by_category"},
    {GrabberSaveMode::OneCategory, "one_category"},
    {GrabberSaveMode::OneFolder, "one_folder"},
}};

constexpr std::array<std::pair<GrabberScheduleMode, std::string_view>, 3> kScheduleModeNames{{
    {GrabberScheduleMode::Explore, "explore"},
    {GrabberScheduleMode::ExploreDownload, "explore_download"},
    {GrabberScheduleMode::DownloadChecked, "download_checked"},
}};

constexpr std::array<std::pair<GrabberItemKind, std::string_view>, 2> kItemKindNames{{
    {GrabberItemKind::Page, "page"},
    {GrabberItemKind::File, "file"},
}};

constexpr std::array<std::pair<GrabberItemState, std::string_view>, 5> kItemStateNames{{
    {GrabberItemState::Found, "found"},
    {GrabberItemState::Filtered, "filtered"},
    {GrabberItemState::Downloading, "downloading"},
    {GrabberItemState::Done, "done"},
    {GrabberItemState::Failed, "failed"},
}};

}  // namespace

std::string_view ToString(GrabberTemplate value) noexcept
{
    return detail::NameOf(kTemplateNames, value);
}

std::optional<GrabberTemplate> ParseGrabberTemplate(std::string_view text) noexcept
{
    return detail::ValueOf(kTemplateNames, text);
}

std::string_view ToString(GrabberSaveMode value) noexcept
{
    return detail::NameOf(kSaveModeNames, value);
}

std::optional<GrabberSaveMode> ParseGrabberSaveMode(std::string_view text) noexcept
{
    return detail::ValueOf(kSaveModeNames, text);
}

std::string_view ToString(GrabberScheduleMode value) noexcept
{
    return detail::NameOf(kScheduleModeNames, value);
}

std::optional<GrabberScheduleMode> ParseGrabberScheduleMode(std::string_view text) noexcept
{
    return detail::ValueOf(kScheduleModeNames, text);
}

std::string_view ToString(GrabberItemKind value) noexcept
{
    return detail::NameOf(kItemKindNames, value);
}

std::optional<GrabberItemKind> ParseGrabberItemKind(std::string_view text) noexcept
{
    return detail::ValueOf(kItemKindNames, text);
}

std::string_view ToString(GrabberItemState value) noexcept
{
    return detail::NameOf(kItemStateNames, value);
}

std::optional<GrabberItemState> ParseGrabberItemState(std::string_view text) noexcept
{
    return detail::ValueOf(kItemStateNames, text);
}

}  // namespace ldm::model
