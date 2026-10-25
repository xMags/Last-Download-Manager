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
    if (item.siteVideo) {
        return L"Video";
    }
    if (item.kind == browser::MediaKind::File) {
        const auto url = core::ParseUrl(item.url);
        std::string type = url ? core::FileExtension(core::FileNameFromUrlPath(url->path)) : std::string();
        std::transform(type.begin(), type.end(), type.begin(),
                       [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
        return type.empty() ? L"Media file" : base::ToUtf16(type) + L" file";
    }
    return item.kind == browser::MediaKind::Hls ? L"HLS stream" : L"DASH stream";
}

PanelMenuItem Command(std::wstring text, PanelAction action, PanelGlyph glyph = PanelGlyph::Download)
{
    PanelMenuItem item;
    item.text = std::move(text);
    item.glyph = glyph;
    item.action = action;
    return item;
}

PanelMenuItem Notice(std::wstring text, PanelGlyph glyph = PanelGlyph::Info)
{
    PanelMenuItem item;
    item.text = std::move(text);
    item.glyph = glyph;
    item.enabled = false;
    return item;
}

PanelMenuItem Submenu(std::wstring text, PanelGlyph glyph)
{
    PanelMenuItem item;
    item.text = std::move(text);
    item.glyph = glyph;
    return item;
}

// The mark video players show for a picture size.
std::wstring SizeBadge(std::optional<int> height)
{
    if (!height) {
        return {};
    }
    if (*height >= 4320) {
        return L"8K";
    }
    if (*height >= 2160) {
        return L"4K";
    }
    return *height >= 720 ? L"HD" : L"";
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
    if (item.siteVideo && item.state == MediaItem::State::Fetching) {
        lines.push_back(Notice(L"Finding qualities..."));
        return lines;
    }
    if (item.siteVideo && item.state == MediaItem::State::Failed) {
        lines.push_back(Notice(base::ToUtf16(item.failure)));
        return lines;
    }
    if (item.qualities.drm) {
        lines.push_back(Notice(DrmText(*item.qualities.drm), PanelGlyph::Lock));
        return lines;
    }
    const bool live = item.offer && item.offer->live;
    if (live && item.kind == browser::MediaKind::Dash && !item.siteVideo) {  // a site's live video is HLS
        lines.push_back(Notice(L"This is a live DASH stream, which LDM cannot record"));
        return lines;
    }
    PanelMenuItem videoOnly = Submenu(L"Video only", PanelGlyph::Video);
    PanelMenuItem audioOnly = Submenu(L"Audio only", PanelGlyph::Audio);
    const auto& entries = item.qualities.entries;
    for (size_t i = 0; i < entries.size(); ++i) {
        const media::QualityEntry& entry = entries[i];
        // A live quality starts a recording, whose size depends on how long it runs.
        const std::wstring label = base::ToUtf16(media::QualityLabel(entry));
        PanelMenuItem line = Command(live ? label + L" (live)" : WithSize(label, entry.approximateBytes, true),
                                     PanelAction{PanelAction::Kind::DownloadStream, item.key, i});
        if (entry.kind == media::QualityKind::Video) {
            line.badge = SizeBadge(entry.height);
            line.quietDetail = entry.height.has_value();  // "1080p" first, the codec after it
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
        PanelMenuItem subtitles = Submenu(L"Subtitles", PanelGlyph::Subtitles);
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
        // The submenu says what is inside, so a protected or live stream shows before it is opened.
        PanelMenuItem stream = Submenu(KindName(*item), PanelGlyph::Video);
        if (item->qualities.drm) {
            stream.text += L" (protected)";
            stream.glyph = PanelGlyph::Lock;
        } else if (!item->qualities.entries.empty()) {
            stream.text += L": " + base::ToUtf16(media::QualityLabel(item->qualities.entries.front()));
            if (item->offer && item->offer->live) {
                stream.text += L" (live)";
            }
        }
        stream.children = std::move(lines);
        menu.push_back(std::move(stream));
    }
    menu.push_back(Separator());
    menu.push_back(mini ? Command(L"Make the panel bigger", PanelAction{PanelAction::Kind::FullPanel}, PanelGlyph::Bigger)
                        : Command(L"Make the panel smaller", PanelAction{PanelAction::Kind::SmallPanel},
                                  PanelGlyph::Smaller));
    menu.push_back(Command(L"Hide the panel for this page", PanelAction{PanelAction::Kind::HideForPage}, PanelGlyph::Hide));
    return menu;
}

}  // namespace ldm::ui
