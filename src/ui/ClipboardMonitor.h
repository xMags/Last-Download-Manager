#pragma once

#include "model/Settings.h"

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>

namespace ldm::ui {

// Decides whether a clipboard change should open Add URL (checklist A-09): the clipboard holds
// one address and nothing else, its file has a captured extension, LDM did not put it there, and
// it was not offered already. UI thread only.
class ClipboardMonitor {
public:
    [[nodiscard]] std::optional<std::string> AddressToOffer(HWND owner, const model::Settings& settings);

private:
    std::string lastOffered_;
};

// The rule behind AddressToOffer, without the clipboard: `text` is what was copied.
[[nodiscard]] std::optional<std::string> OfferableAddress(std::string_view text, std::string_view capturedExtensions);

}  // namespace ldm::ui
