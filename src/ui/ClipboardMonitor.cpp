#include "ui/ClipboardMonitor.h"

#include "base/Utf.h"
#include "core/FileNames.h"
#include "core/Matching.h"
#include "core/Url.h"
#include "ui/Clipboard.h"

namespace ldm::ui {

namespace {

// A copied link is short; longer clipboard texts are documents, not addresses.
constexpr size_t kMaxAddressChars = 4096;

std::string_view Trim(std::string_view text)
{
    constexpr std::string_view kSpace = " \t\r\n";
    const size_t first = text.find_first_not_of(kSpace);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(kSpace) - first + 1);
}

}  // namespace

std::optional<std::string> OfferableAddress(std::string_view text, std::string_view capturedExtensions)
{
    const std::string_view address = Trim(text);
    if (address.empty() || address.find_first_of(" \t\r\n") != std::string_view::npos) {
        return std::nullopt;
    }
    const auto url = core::ParseUrl(address);
    if (!url) {
        return std::nullopt;
    }
    const std::string extension = core::FileExtension(core::FileNameFromUrlPath(url->path));
    if (!core::MatchesExtensionList(capturedExtensions, extension)) {
        return std::nullopt;
    }
    return std::string(address);
}

std::optional<std::string> ClipboardMonitor::AddressToOffer(HWND owner, const model::Settings& settings)
{
    if (!settings.monitorClipboard) {
        return std::nullopt;
    }
    // Copy Address in LDM's own menus must not bounce back as an offer.
    if (const HWND clipboardOwner = ::GetClipboardOwner(); clipboardOwner != nullptr) {
        DWORD process = 0;
        ::GetWindowThreadProcessId(clipboardOwner, &process);
        if (process == ::GetCurrentProcessId()) {
            return std::nullopt;
        }
    }
    auto address = OfferableAddress(base::ToUtf8(ReadClipboardText(owner, kMaxAddressChars)), settings.capturedExtensions);
    if (!address || *address == lastOffered_) {
        return std::nullopt;
    }
    lastOffered_ = *address;
    return address;
}

}  // namespace ldm::ui
