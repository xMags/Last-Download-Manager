#include "ui/GrabberFields.h"

#include "base/Utf.h"
#include "core/Url.h"

#include <cwctype>

namespace ldm::ui {

std::optional<std::vector<std::string>> ParseMasks(std::wstring_view text)
{
    std::vector<std::string> masks;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::iswspace(text[i])) {
            ++i;
        }
        const std::size_t begin = i;
        while (i < text.size() && !std::iswspace(text[i])) {
            ++i;
        }
        if (i == begin) {
            break;
        }
        std::string mask = base::ToUtf8(text.substr(begin, i - begin));
        if (mask.size() > model::kMaxGrabberMaskLength || masks.size() >= model::kMaxGrabberMasks) {
            return std::nullopt;
        }
        masks.push_back(std::move(mask));
    }
    return masks;
}

std::wstring JoinMasks(const std::vector<std::string>& masks)
{
    std::wstring text;
    for (const std::string& mask : masks) {
        if (!text.empty()) {
            text += L' ';
        }
        text += base::ToUtf16(mask);
    }
    return text;
}

std::optional<std::string> ParseStartPage(std::wstring_view text)
{
    while (!text.empty() && std::iswspace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::iswspace(text.back())) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::string address = base::ToUtf8(text);
    if (address.find("://") == std::string::npos) {
        address = "https://" + address;
    }
    const auto url = core::ParseUrl(address);
    if (!url || (url->scheme != "http" && url->scheme != "https")) {
        return std::nullopt;
    }
    return address;
}

const std::vector<TemplateChoice>& TemplateChoices()
{
    static const std::vector<TemplateChoice> choices{
        {model::GrabberTemplate::Custom, L"Custom",
         L"Choose the pages to explore and the files to list yourself, on the next pages."},
        {model::GrabberTemplate::Pictures, L"All pictures on the site", L"Explores the whole site and lists its pictures."},
        {model::GrabberTemplate::Audio, L"Audio on the start page and one level below",
         L"Lists the audio files the start page and the pages it links to point at."},
        {model::GrabberTemplate::Video, L"All video on the site", L"Explores the whole site and lists its video files."},
        {model::GrabberTemplate::AllFiles, L"All files on the start page and one level below",
         L"Lists every file that is not a web page, on the start page and the pages it links to."},
        {model::GrabberTemplate::WholeSite, L"The whole site, for offline browsing",
         L"Saves every page of the site with its files, linked to each other on disk."},
        {model::GrabberTemplate::StartPageOffline, L"The start page, for offline browsing",
         L"Saves the start page with its pictures, styles and scripts, linked on disk."},
    };
    return choices;
}

std::wstring SuggestedProjectName(std::string_view startPage)
{
    const auto url = core::ParseUrl(startPage);
    if (!url || url->host.empty()) {
        return L"New project";
    }
    std::string host = url->host;
    if (host.starts_with("www.")) {
        host.erase(0, 4);
    }
    return base::ToUtf16(host);
}

}  // namespace ldm::ui
