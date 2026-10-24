#pragma once

#include "browser/Protocol.h"

#include <cstdint>
#include <optional>
#include <string>

namespace ldm::ui {

// One connected extension (one browser profile), as numbered by the browser link. Never reused
// while LDM runs, so a late answer cannot reach a different browser.
using BrowserConnectionId = std::uint64_t;

// Something that happened on a browser connection, delivered to the UI thread.
struct BrowserEvent {
    BrowserConnectionId connection = 0;
    std::optional<browser::ParsedMessage> message;  // nothing: the connection closed
    // The process at the other end of the pipe (ldm-host.exe), as Windows reports it; 0 when
    // unknown. LDM finds the browser's windows through it, for the media panel.
    std::uint32_t hostProcessId = 0;
};

// Sends messages to connected extensions (M3 design section 3). Implemented in platform/win.
class BrowserLink {
public:
    virtual ~BrowserLink() = default;
    // Queues `json` for the connection; dropped silently when it has closed meanwhile.
    // Safe to call from any thread.
    virtual void Send(BrowserConnectionId connection, std::string json) = 0;
};

}  // namespace ldm::ui
