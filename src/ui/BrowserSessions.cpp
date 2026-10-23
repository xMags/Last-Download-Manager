#include "ui/BrowserSessions.h"

#include "core/Url.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>
#include <vector>

namespace ldm::ui {

namespace {

// Pushed configs are not answers to a request.
constexpr char kPushId[] = "push";

// Every setting MakeConfig reads, joined, to notice when the extensions need a new config.
std::string ConfigInputs(const model::Settings& settings)
{
    constexpr char kSeparator = '\x1f';
    const model::CaptureKeys& k = settings.captureKeys;
    std::string keys;
    for (const bool flag : {k.preventEnabled, k.preventAlt, k.preventShift, k.preventCtrl, k.forceEnabled, k.forceAlt,
                            k.forceShift, k.forceCtrl, k.forceInsert, k.forceDelete, k.forceNeedsLinkClick,
                            k.skipWebContent, settings.chromiumMenu.downloadLink, settings.chromiumMenu.downloadAll,
                            settings.firefoxMenu.downloadLink, settings.firefoxMenu.downloadAll}) {
        keys += flag ? '1' : '0';
    }
    return settings.capturedExtensions + kSeparator + settings.captureSiteExceptions + kSeparator +
           settings.captureAddressExceptions + kSeparator + keys;
}

std::vector<std::string> Words(const std::string& text)
{
    std::vector<std::string> words;
    std::istringstream stream(text);
    for (std::string word; stream >> word;) {
        words.push_back(std::move(word));
    }
    return words;
}

}  // namespace

BrowserSessions::BrowserSessions(BrowserLink& link, std::string appVersion, Handlers handlers)
    : link_(link), appVersion_(std::move(appVersion)), handlers_(std::move(handlers))
{
}

void BrowserSessions::OnEvent(BrowserEvent event, const model::Settings& settings)
{
    if (!event.message) {
        if (sessions_.erase(event.connection) > 0 && handlers_.onClosed) {
            handlers_.onClosed(event.connection);
        }
        return;
    }
    browser::ParsedMessage& parsed = *event.message;
    if (auto* host = std::get_if<browser::HostInfo>(&parsed.message)) {
        sessions_[event.connection] = Session{std::move(*host), std::nullopt};
        return;
    }
    const auto session = sessions_.find(event.connection);
    if (session == sessions_.end()) {
        return;  // the bridge guarantees the host message first; ignore anything else
    }
    if (auto* hello = std::get_if<browser::Hello>(&parsed.message)) {
        session->second.hello = std::move(*hello);
        link_.Send(event.connection, browser::EncodeConfig(MakeConfig(settings, session->second.host.family), parsed.id));
        return;
    }
    if (auto* capture = std::get_if<browser::Capture>(&parsed.message)) {
        // Accepting means LDM owns the download from here: the extension cancels the browser's
        // copy. Anything LDM could not download is refused, so the browser keeps it.
        const bool accepted = session->second.hello.has_value() && core::ParseUrl(capture->url).has_value();
        link_.Send(event.connection, browser::EncodeCaptureResult(parsed.id, accepted));
        if (accepted && handlers_.onCapture) {
            handlers_.onCapture(std::move(*capture));
        }
        return;
    }
    if (auto* links = std::get_if<browser::Links>(&parsed.message)) {
        // The dialog opens when at least one link is an address LDM can download.
        const bool usable = std::any_of(links->links.begin(), links->links.end(),
                                        [](const browser::PageLink& l) { return core::ParseUrl(l.url).has_value(); });
        const bool accepted = session->second.hello.has_value() && usable && handlers_.onLinks;
        link_.Send(event.connection, browser::EncodeLinksResult(parsed.id, accepted));
        if (accepted) {
            handlers_.onLinks(LinksRequest{event.connection, parsed.id, std::move(*links)});
        }
        return;
    }
    if (auto* info = std::get_if<browser::LinkInfo>(&parsed.message)) {
        if (session->second.hello && handlers_.onLinkInfo) {
            handlers_.onLinkInfo(event.connection, std::move(*info));
        }
    }
}

void BrowserSessions::LinksDone(BrowserConnectionId connection, const std::string& requestId)
{
    if (sessions_.contains(connection)) {
        link_.Send(connection, browser::EncodeLinksDone(requestId));
    }
}

void BrowserSessions::OnSettingsChanged(const model::Settings& settings)
{
    // Snapshots arrive up to 10 times a second; settings rarely change.
    std::string inputs = ConfigInputs(settings);
    if (configInputs_ == inputs) {
        return;
    }
    // The first call only records the starting point; nobody has a stale copy yet.
    const bool first = !configInputs_.has_value();
    configInputs_ = std::move(inputs);
    if (first) {
        return;
    }
    for (const auto& [connection, session] : sessions_) {
        if (session.hello) {
            link_.Send(connection, browser::EncodeConfig(MakeConfig(settings, session.host.family), kPushId));
        }
    }
}

browser::Config BrowserSessions::MakeConfig(const model::Settings& settings, browser::BrowserFamily family) const
{
    browser::Config config;
    config.appVersion = appVersion_;
    for (std::string word : Words(settings.capturedExtensions)) {
        for (char& c : word) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        config.capturedExtensions.push_back(std::move(word));
    }
    config.siteExceptions = Words(settings.captureSiteExceptions);
    // Address patterns hold no spaces, so whitespace separates them as well as line breaks do.
    config.addressExceptions = Words(settings.captureAddressExceptions);
    config.keys = settings.captureKeys;
    config.menu = family == browser::BrowserFamily::Firefox ? settings.firefoxMenu : settings.chromiumMenu;
    return config;
}

}  // namespace ldm::ui
