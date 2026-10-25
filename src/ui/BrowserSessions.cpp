#include "ui/BrowserSessions.h"

#include "core/SiteList.h"
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
                            settings.firefoxMenu.downloadLink, settings.firefoxMenu.downloadAll,
                            settings.mediaPanelEnabled, settings.siteVideosEnabled}) {
        keys += flag ? '1' : '0';
    }
    return settings.capturedExtensions + kSeparator + settings.captureSiteExceptions + kSeparator +
           settings.captureAddressExceptions + kSeparator + keys + kSeparator + settings.mediaTypes + kSeparator +
           std::to_string(settings.mediaMinimumKilobytes) + kSeparator + settings.mediaSiteExceptions;
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
        // Answered after the bookkeeping, since an answer may send a new request.
        std::vector<CookiesAnswer> unanswered;
        for (auto it = cookieRequests_.begin(); it != cookieRequests_.end();) {
            if (it->second.connection == event.connection) {
                unanswered.push_back(std::move(it->second.answer));
                it = cookieRequests_.erase(it);
            } else {
                ++it;
            }
        }
        if (sessions_.erase(event.connection) > 0 && handlers_.onClosed) {
            handlers_.onClosed(event.connection);
        }
        for (CookiesAnswer& answer : unanswered) {
            answer(std::nullopt);
        }
        return;
    }
    browser::ParsedMessage& parsed = *event.message;
    if (auto* host = std::get_if<browser::HostInfo>(&parsed.message)) {
        sessions_[event.connection] = Session{std::move(*host), std::nullopt, event.hostProcessId};
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
        return;
    }
    if (auto* cookies = std::get_if<browser::Cookies>(&parsed.message)) {
        const auto request = cookieRequests_.find(cookies->requestId);
        // Only the connection that was asked may answer.
        if (request != cookieRequests_.end() && request->second.connection == event.connection) {
            CookiesAnswer answer = std::move(request->second.answer);
            cookieRequests_.erase(request);
            answer(std::move(cookies->cookies));
        }
        return;
    }
    if (!session->second.hello) {
        return;  // media reports start after the extension's hello
    }
    if (auto* media = std::get_if<browser::MediaFound>(&parsed.message)) {
        if (handlers_.onMedia) {
            handlers_.onMedia(event.connection, std::move(*media));
        }
    } else if (const auto* reset = std::get_if<browser::MediaReset>(&parsed.message)) {
        if (handlers_.onMediaReset) {
            handlers_.onMediaReset(event.connection, reset->tabId);
        }
    } else if (auto* page = std::get_if<browser::SitePage>(&parsed.message)) {
        // Asked for only while site videos are on; one sent just before they were turned off is dropped.
        if (handlers_.onSitePage && settings.siteVideosEnabled) {
            handlers_.onSitePage(event.connection, std::move(*page));
        }
    } else if (const auto* position = std::get_if<browser::VideoPosition>(&parsed.message)) {
        if (handlers_.onVideoPosition) {
            handlers_.onVideoPosition(event.connection, *position);
        }
    }
}

std::optional<BrowserConnectionId> BrowserSessions::PreferredConnection() const
{
    // A browser's lasting connection asks LDM not to start; the short ones of single requests ask
    // it to and close soon, so a message sent there could be lost.
    // Connection ids grow, so the last match is the newest.
    std::optional<BrowserConnectionId> lasting;
    std::optional<BrowserConnectionId> any;
    for (const auto& [connection, session] : sessions_) {
        if (!session.hello) {
            continue;
        }
        any = connection;
        if (!session.hello->startLdm) {
            lasting = connection;
        }
    }
    return lasting ? lasting : any;
}

bool BrowserSessions::OpenPage(const std::string& url)
{
    const auto parsed = core::ParseUrl(url);
    if (!parsed || (parsed->scheme != "http" && parsed->scheme != "https")) {
        return false;
    }
    const std::optional<BrowserConnectionId> target = PreferredConnection();
    if (!target) {
        return false;
    }
    link_.Send(*target, browser::EncodeOpenPage(url));
    return true;
}

bool BrowserSessions::RequestCookies(const std::string& url, CookiesAnswer answer,
                                     std::chrono::steady_clock::time_point deadline)
{
    const auto parsed = core::ParseUrl(url);
    if (!parsed || (parsed->scheme != "http" && parsed->scheme != "https")) {
        return false;
    }
    const std::optional<BrowserConnectionId> target = PreferredConnection();
    if (!target) {
        return false;
    }
    std::string requestId = "cookies-" + std::to_string(++lastCookieRequest_);
    link_.Send(*target, browser::EncodeCookiesRequest(requestId, url));
    cookieRequests_.emplace(std::move(requestId), CookieRequest{*target, std::move(answer), deadline});
    return true;
}

void BrowserSessions::ExpireCookieRequests(std::chrono::steady_clock::time_point now)
{
    std::vector<CookiesAnswer> expired;
    for (auto it = cookieRequests_.begin(); it != cookieRequests_.end();) {
        if (it->second.deadline <= now) {
            expired.push_back(std::move(it->second.answer));
            it = cookieRequests_.erase(it);
        } else {
            ++it;
        }
    }
    for (CookiesAnswer& answer : expired) {
        answer(std::nullopt);
    }
}

void BrowserSessions::SendMediaList(BrowserConnectionId connection, int tabId, bool hasMedia)
{
    if (sessions_.contains(connection)) {
        link_.Send(connection, browser::EncodeMediaList(tabId, hasMedia));
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
    const auto lowerWords = [](const std::string& text) {
        std::vector<std::string> words = Words(text);
        for (std::string& word : words) {
            for (char& c : word) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
        }
        return words;
    };
    browser::Config config;
    config.appVersion = appVersion_;
    config.capturedExtensions = lowerWords(settings.capturedExtensions);
    config.media.enabled = settings.mediaPanelEnabled;
    config.media.types = lowerWords(settings.mediaTypes);
    config.media.minimumBytes = std::int64_t{settings.mediaMinimumKilobytes} * 1024;
    config.media.siteExceptions = lowerWords(settings.mediaSiteExceptions);
    config.sitePages = settings.siteVideosEnabled;
    config.siteHosts = core::AutomaticSiteHosts();
    config.siteExceptions = Words(settings.captureSiteExceptions);
    // Address patterns hold no spaces, so whitespace separates them as well as line breaks do.
    config.addressExceptions = Words(settings.captureAddressExceptions);
    config.keys = settings.captureKeys;
    config.menu = family == browser::BrowserFamily::Firefox ? settings.firefoxMenu : settings.chromiumMenu;
    return config;
}

}  // namespace ldm::ui
