#include "ui/GrabberView.h"

#include "base/Utf.h"
#include "core/FileNames.h"
#include "core/Url.h"
#include "ui/Format.h"

#include <algorithm>
#include <cwctype>

namespace ldm::ui {

namespace {

std::wstring Upper(std::string_view text)
{
    std::wstring upper = base::ToUtf16(text);
    std::transform(upper.begin(), upper.end(), upper.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    return upper;
}

std::wstring Count(int count, const wchar_t* one, const wchar_t* many)
{
    return std::to_wstring(count) + L" " + (count == 1 ? one : many);
}

}  // namespace

bool InGrabberNode(const model::GrabberItem& item, const GrabberNode& node, const std::string& startHost)
{
    const bool file = item.kind == model::GrabberItemKind::File;
    const bool leftOut = item.state == model::GrabberItemState::Filtered;
    switch (node.kind) {
    case GrabberNode::Kind::AllFiles:
        return file && !leftOut;
    case GrabberNode::Kind::Checked:
        return file && !leftOut && item.checked;
    case GrabberNode::Kind::Downloaded:
        return file && item.state == model::GrabberItemState::Done;
    case GrabberNode::Kind::Failed:
        return file && item.state == model::GrabberItemState::Failed;
    case GrabberNode::Kind::Type:
        return file && !leftOut && GrabberItemType(item) == node.type;
    case GrabberNode::Kind::OtherSites: {
        if (!file || leftOut) {
            return false;
        }
        const auto url = core::ParseUrl(item.url);
        return url && url->host != startHost;
    }
    case GrabberNode::Kind::LeftOut:
        return file && leftOut;
    case GrabberNode::Kind::Pages:
        return !file;
    }
    return false;
}

std::vector<std::wstring> GrabberTypes(const std::vector<std::shared_ptr<const model::GrabberItem>>& items)
{
    std::vector<std::wstring> types;
    for (const auto& item : items) {
        if (item->kind == model::GrabberItemKind::File && item->state != model::GrabberItemState::Filtered) {
            types.push_back(GrabberItemType(*item));
        }
    }
    std::sort(types.begin(), types.end());
    types.erase(std::unique(types.begin(), types.end()), types.end());
    return types;
}

std::wstring GrabberItemName(const model::GrabberItem& item)
{
    const auto url = core::ParseUrl(item.url);
    if (!url) {
        return base::ToUtf16(item.url);
    }
    std::string name = core::FileNameFromUrlPath(url->path);
    if (name.empty()) {
        name = url->path == "/" ? url->host : url->path;
    }
    if (!url->query.empty() && item.kind == model::GrabberItemKind::Page) {
        name += "?" + url->query;
    }
    return base::ToUtf16(name);
}

std::wstring GrabberItemType(const model::GrabberItem& item)
{
    if (item.kind == model::GrabberItemKind::Page) {
        return L"Page";
    }
    const auto url = core::ParseUrl(item.url);
    return url ? Upper(core::FileExtension(core::FileNameFromUrlPath(url->path))) : std::wstring();
}

std::wstring GrabberItemStatus(const model::GrabberItem& item, const core::DownloadView* download)
{
    if (item.kind == model::GrabberItemKind::Page) {
        switch (item.state) {
        case model::GrabberItemState::Done:
            return L"Explored";
        case model::GrabberItemState::Failed:
            return L"Could not be read";
        default:
            return L"Not explored";
        }
    }
    switch (item.state) {
    case model::GrabberItemState::Found:
        return std::wstring();
    case model::GrabberItemState::Filtered:
        return L"Left out by the filters";
    case model::GrabberItemState::Downloading:
        return download != nullptr ? FormatStatus(*download) : L"Downloading";
    case model::GrabberItemState::Done:
        return L"Complete";
    case model::GrabberItemState::Failed:
        return L"Failed";
    }
    return std::wstring();
}

std::wstring GrabberStatusText(const grabber::GrabberProjectView& view)
{
    if (view.damaged) {
        return L"The project's settings are damaged; delete it and make it again.";
    }
    const grabber::GrabberStats& s = view.stats;
    std::wstring text;
    if (view.startPageFailure && !view.exploring) {
        return L"The start page could not be read: " + base::ToUtf16(*view.startPageFailure) +
               L". Check its address, or sign in to the site in the project's settings.";
    }
    if (view.exploring) {
        text = L"Exploring: " + Count(s.pagesExplored, L"page", L"pages") + L" read, " + std::to_wstring(s.pagesWaiting) +
               L" waiting. ";
    } else if (view.downloading) {
        text = L"Downloading. ";
    }
    text += Count(s.filesFound, L"file", L"files") + L" listed, " + std::to_wstring(s.filesChecked) + L" checked, " +
            std::to_wstring(s.filesDone) + L" downloaded";
    if (s.bytesDone > 0) {
        text += L" (" + FormatSize(s.bytesDone) + L")";
    }
    if (s.filesFailed > 0) {
        text += L", " + std::to_wstring(s.filesFailed) + L" failed";
    }
    text += L".";
    if (s.limitReached) {
        text += L" The site was larger than the Grabber explores at once; some pages were not read.";
    }
    return text;
}

std::wstring GrabberStatisticsText(const grabber::GrabberProjectView& view)
{
    const grabber::GrabberStats& s = view.stats;
    std::wstring text = base::ToUtf16(view.project.name) + L"\n\n";
    text += L"Pages read in the last exploration: " + std::to_wstring(s.pagesExplored) + L"\n";
    text += L"Pages that could not be read: " + std::to_wstring(s.pagesFailed) + L"\n";
    text += L"Files listed: " + std::to_wstring(s.filesFound) + L"\n";
    text += L"Files checked: " + std::to_wstring(s.filesChecked) + L"\n";
    text += L"Files downloaded: " + std::to_wstring(s.filesDone) + L" (" + FormatSize(s.bytesDone) + L")\n";
    text += L"Files that failed: " + std::to_wstring(s.filesFailed) + L"\n";
    text += L"Last run: " + (view.project.lastRunAtMs ? FormatDateTime(view.project.lastRunAtMs) : std::wstring(L"never"));
    return text;
}

}  // namespace ldm::ui
