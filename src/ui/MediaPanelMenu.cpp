#include "ui/MediaPanelMenu.h"

#include "base/Utf.h"
#include "core/FileNames.h"
#include "core/Url.h"
#include "media/QualityList.h"
#include "ui/Format.h"

#include <algorithm>
#include <cctype>

namespace ldm::ui {

namespace {

std::wstring WithSize(std::wstring text, std::optional<std::int64_t> bytes, bool approximate)
{
    if (bytes && *bytes > 0) {
        text += L"\t";
        text += approximate ? L"~" : L"";
        text += FormatSize(*bytes);
    }
    return text;
}

std::wstring KindName(const MediaItem& item)
{
    if (item.kind == browser::MediaKind::File) {
        const auto url = core::ParseUrl(item.url);
        std::string type = url ? core::FileExtension(core::FileNameFromUrlPath(url->path)) : std::string();
        std::transform(type.begin(), type.end(), type.begin(),
                       [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
        return type.empty() ? L"Media file" : base::ToUtf16(type) + L" file";
    }
    return item.kind == browser::MediaKind::Hls ? L"HLS stream" : L"DASH stream";
}

PanelMenuItem Command(std::wstring text, PanelAction action)
{
    PanelMenuItem item;
    item.text = std::move(text);
    item.action = action;
    return item;
}

PanelMenuItem Notice(std::wstring text)
{
    PanelMenuItem item;
    item.text = std::move(text);
    item.enabled = false;
    return item;
}

PanelMenuItem Separator()
{
    PanelMenuItem item;
    item.separator = true;
    return item;
}

std::wstring DrmText(const media::DrmInfo& drm)
{
    std::wstring systems;
    for (const std::string& system : drm.systems) {
        systems += (systems.empty() ? L"" : L", ") + base::ToUtf16(system);
    }
    return L"This video is protected" + (systems.empty() ? std::wstring() : L" (" + systems + L")") +
           L" and cannot be downloaded";
}

// A stream's lines: the qualities with sound, then submenus for the rest.
std::vector<PanelMenuItem> StreamLines(const MediaItem& item)
{
    std::vector<PanelMenuItem> lines;
    if (item.qualities.drm) {
        lines.push_back(Notice(DrmText(*item.qualities.drm)));
        return lines;
    }
    PanelMenuItem videoOnly;
    videoOnly.text = L"Video only";
    PanelMenuItem audioOnly;
    audioOnly.text = L"Audio only";
    const auto& entries = item.qualities.entries;
    for (size_t i = 0; i < entries.size(); ++i) {
        const media::QualityEntry& entry = entries[i];
        PanelMenuItem line = Command(WithSize(base::ToUtf16(media::QualityLabel(entry)), entry.approximateBytes, true),
                                     PanelAction{PanelAction::Kind::DownloadStream, item.key, i});
        if (entry.kind == media::QualityKind::Video) {
            lines.push_back(std::move(line));
        } else if (entry.kind == media::QualityKind::VideoOnly) {
            videoOnly.children.push_back(std::move(line));
        } else {
            audioOnly.children.push_back(std::move(line));
        }
    }
    if (lines.empty()) {
        // Sound only: no submenu needed.
        lines = std::move(audioOnly.children);
        audioOnly.children.clear();
    }
    for (PanelMenuItem* group : {&videoOnly, &audioOnly}) {
        if (!group->children.empty()) {
            lines.push_back(std::move(*group));
        }
    }
    if (!item.qualities.subtitles.empty() && item.offer) {
        PanelMenuItem subtitles;
        subtitles.text = L"Subtitles";
        for (size_t i = 0; i < item.qualities.subtitles.size(); ++i) {
            const media::Rendition& rendition = item.offer->subtitles[item.qualities.subtitles[i]];
            std::string name = rendition.name;
            if (!rendition.language.empty() && rendition.language != rendition.name) {
                name += " (" + rendition.language + ")";
            }
            subtitles.children.push_back(
                Command(base::ToUtf16(name), PanelAction{PanelAction::Kind::DownloadSubtitles, item.key, i}));
        }
        lines.push_back(std::move(subtitles));
    }
    return lines;
}

}  // namespace

std::vector<PanelMenuItem> BuildPanelMenu(const std::vector<const MediaItem*>& offered, bool mini)
{
    std::vector<PanelMenuItem> menu;
    const size_t streams = static_cast<size_t>(std::count_if(offered.begin(), offered.end(), [](const MediaItem* item) {
        return item->kind != browser::MediaKind::File;
    }));
    for (const MediaItem* item : offered) {
        if (item->kind == browser::MediaKind::File) {
            menu.push_back(Command(WithSize(L"Download " + KindName(*item), item->fileSize, false),
                                   PanelAction{PanelAction::Kind::DownloadFile, item->key, 0}));
            continue;
        }
        std::vector<PanelMenuItem> lines = StreamLines(*item);
        if (streams == 1) {
            for (PanelMenuItem& line : lines) {
                menu.push_back(std::move(line));
            }
            continue;
        }
        PanelMenuItem stream;
        stream.text = KindName(*item);
        if (!item->qualities.entries.empty()) {
            stream.text += L": " + base::ToUtf16(media::QualityLabel(item->qualities.entries.front()));
        }
        stream.children = std::move(lines);
        menu.push_back(std::move(stream));
    }
    menu.push_back(Separator());
    menu.push_back(mini ? Command(L"Make the panel bigger", PanelAction{PanelAction::Kind::FullPanel})
                        : Command(L"Make the panel smaller", PanelAction{PanelAction::Kind::SmallPanel}));
    menu.push_back(Command(L"Hide the panel for this page", PanelAction{PanelAction::Kind::HideForPage}));
    return menu;
}

}  // namespace ldm::ui
