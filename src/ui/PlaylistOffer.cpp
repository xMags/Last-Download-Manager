#include "ui/PlaylistOffer.h"

#include "base/Utf.h"
#include "core/FileNames.h"
#include "ui/SmallFile.h"
#include "ui/dialogs/MessageDialog.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <utility>

namespace ldm::ui {

namespace {

// A playlist of links is small; a larger file is not offered, and reading it stays quick.
constexpr size_t kMaxPlaylistBytes = 1024 * 1024;

}  // namespace

bool IsPlaylistFileName(std::string_view name)
{
    std::string extension = core::FileExtension(std::string(name));
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == "m3u" || extension == "m3u8";
}

PlaylistOffer::PlaylistOffer(Context context) : context_(std::move(context)) {}

void PlaylistOffer::OnCompleted(const core::DownloadCompleted& completed)
{
    if (!IsPlaylistFileName(completed.fileName)) {
        return;
    }
    const auto text = ReadSmallFile(std::filesystem::path(base::ToUtf16(completed.finalPath)), kMaxPlaylistBytes);
    if (!text) {
        return;
    }
    std::vector<media::M3uEntry> entries = media::ParseM3uLinks(*text, completed.url);
    if (entries.empty()) {
        return;
    }
    waiting_.push_back(Offer{completed.fileName, completed.url, std::move(entries)});
    OfferWaiting();
}

void PlaylistOffer::OfferWaiting()
{
    if (offering_) {
        return;
    }
    offering_ = true;
    // A disabled owner means a modal dialog is open; the offer waits for it to close.
    while (!waiting_.empty() && ::IsWindowEnabled(context_.owner) != FALSE) {
        const Offer offer = std::move(waiting_.front());
        waiting_.pop_front();
        const size_t count = offer.entries.size();
        const std::wstring question = base::ToUtf16(offer.fileName) + L" is a playlist with " + std::to_wstring(count) +
                                      (count == 1 ? L" link" : L" links") + L". Add them to the download list?";
        const int answer = ShowMessage(context_.owner, *context_.theme,
                                       MessageRequest{question, MessageKind::Question, MessageButtons::YesNo, true});
        if (answer == IDYES && context_.addLinks) {
            context_.addLinks(offer.entries, offer.url);
        }
    }
    offering_ = false;
}

}  // namespace ldm::ui
