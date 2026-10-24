#pragma once

#include "browser/Protocol.h"
#include "model/Settings.h"
#include "ui/BrowserLink.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ldm::ui {

// The extensions connected right now and what LDM tells them (M3 design sections 3 to 5).
// UI thread only. Answers each hello with the capture rules, and pushes them again when the
// settings change.
class BrowserSessions {
public:
    struct Session {
        browser::HostInfo host;
        std::optional<browser::Hello> hello;  // nothing until the extension said hello
        std::uint32_t hostProcessId = 0;      // ldm-host.exe of this connection; 0 when unknown
    };

    // A page's links from the browser's right-click menu, and where to send links-done.
    struct LinksRequest {
        BrowserConnectionId connection = 0;
        std::string requestId;
        browser::Links links;
    };

    // Each runs inside OnEvent. onCapture and onLinks must only queue, because a dialog may be
    // open; onLinkInfo feeds the open Download All Links dialog.
    struct Handlers {
        std::function<void(browser::Capture)> onCapture;
        std::function<void(LinksRequest)> onLinks;
        std::function<void(BrowserConnectionId, browser::LinkInfo)> onLinkInfo;
        std::function<void(BrowserConnectionId)> onClosed;  // a connection that had said host ended
        // Media panel (M3 design section 6), after hello only.
        std::function<void(BrowserConnectionId, browser::MediaFound)> onMedia;
        std::function<void(BrowserConnectionId, int tabId)> onMediaReset;
        std::function<void(BrowserConnectionId, browser::VideoPosition)> onVideoPosition;
    };

    BrowserSessions(BrowserLink& link, std::string appVersion, Handlers handlers);

    void OnEvent(BrowserEvent event, const model::Settings& settings);
    void OnSettingsChanged(const model::Settings& settings);
    // The Download All Links dialog for this request closed.
    void LinksDone(BrowserConnectionId connection, const std::string& requestId);
    // Opens `url` (http or https) in a tab of the browser that connected last; false when no
    // browser is connected.
    [[nodiscard]] bool OpenPage(const std::string& url);
    // Tells the extension of `connection` whether LDM has media to offer for a tab.
    void SendMediaList(BrowserConnectionId connection, int tabId, bool hasMedia);

    // The cookies a browser would send to an address (the Site Grabber, M4 design section 6.4):
    // nothing when its connection closed or the request expired first.
    using CookiesAnswer = std::function<void(std::optional<std::vector<model::Cookie>>)>;
    // Asks the browser that connected last for the cookies it sends to `url` (http or https).
    // False when no browser is connected; otherwise `answer` runs exactly once, inside OnEvent or
    // ExpireCookieRequests.
    [[nodiscard]] bool RequestCookies(const std::string& url, CookiesAnswer answer,
                                      std::chrono::steady_clock::time_point deadline);
    // Answers the requests whose deadline passed with nothing.
    void ExpireCookieRequests(std::chrono::steady_clock::time_point now);

    [[nodiscard]] const std::map<BrowserConnectionId, Session>& Sessions() const noexcept { return sessions_; }

private:
    [[nodiscard]] browser::Config MakeConfig(const model::Settings& settings, browser::BrowserFamily family) const;
    // The connection to send a request on: the newest lasting one, else the newest of any.
    [[nodiscard]] std::optional<BrowserConnectionId> PreferredConnection() const;

    struct CookieRequest {
        BrowserConnectionId connection = 0;
        CookiesAnswer answer;
        std::chrono::steady_clock::time_point deadline;
    };
    std::map<std::string, CookieRequest> cookieRequests_;  // by request id
    std::uint64_t lastCookieRequest_ = 0;

    BrowserLink& link_;
    std::string appVersion_;
    Handlers handlers_;
    std::map<BrowserConnectionId, Session> sessions_;
    // The settings MakeConfig reads (ConfigInputs), as last pushed.
    std::optional<std::string> configInputs_;
};

}  // namespace ldm::ui
