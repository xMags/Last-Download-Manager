#pragma once

#include "model/GrabberProject.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What the Grabber's wizard and window show of a project, as text, and what they read back.
// Pure, so the rules are tested without the dialogs.
namespace ldm::ui {

// Masks typed as one line, separated by spaces. Nothing when there are more than
// model::kMaxGrabberMasks or one is longer than model::kMaxGrabberMaskLength.
[[nodiscard]] std::optional<std::vector<std::string>> ParseMasks(std::wstring_view text);
[[nodiscard]] std::wstring JoinMasks(const std::vector<std::string>& masks);

// The start page as typed: surrounding spaces removed and "https://" added when no scheme was
// typed. Nothing unless it is an http or https address.
[[nodiscard]] std::optional<std::string> ParseStartPage(std::wstring_view text);

// The wizard's template list, in its order, with a line about each.
struct TemplateChoice {
    model::GrabberTemplate kind = model::GrabberTemplate::Custom;
    const wchar_t* name = L"";
    const wchar_t* description = L"";
};
[[nodiscard]] const std::vector<TemplateChoice>& TemplateChoices();

// A suggested project name for a start page: its host, or "New project".
[[nodiscard]] std::wstring SuggestedProjectName(std::string_view startPage);

}  // namespace ldm::ui
