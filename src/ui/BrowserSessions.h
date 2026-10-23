#pragma once

#include "browser/Protocol.h"
#include "model/Settings.h"
#include "ui/BrowserLink.h"

#include <map>
#include <optional>
#include <string>

namespace ldm::ui {

// The extensions connected right now and what LDM tells them (M3 design sections 3 to 5).
// UI thread only. Answers each hello with the capture rules, and pushes them again when the
// settings change.
class BrowserSessions {
public:
    struct Session {
        browser::HostInfo host;
        std::optional<browser::Hello> hello;  // nothing until the extension said hello
    };

    BrowserSessions(BrowserLink& link, std::string appVersion);

    void OnEvent(BrowserEvent event, const model::Settings& settings);
    void OnSettingsChanged(const model::Settings& settings);

    [[nodiscard]] const std::map<BrowserConnectionId, Session>& Sessions() const noexcept { return sessions_; }

private:
    [[nodiscard]] browser::Config MakeConfig(const model::Settings& settings) const;

    BrowserLink& link_;
    std::string appVersion_;
    std::map<BrowserConnectionId, Session> sessions_;
    // The settings MakeConfig reads, as last pushed; extend together with MakeConfig.
    std::optional<std::string> configInputs_;
};

}  // namespace ldm::ui
