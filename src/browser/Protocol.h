#pragma once

#include "model/Cookie.h"
#include "model/Settings.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Messages between the LDM browser extension, ldm-host.exe and ldm.exe (M3 design section 4).
// Every message is a JSON object {"v": 1, "id": "...", "type": "...", ...}. Everything that
// arrives is untrusted: the extension relays data from web pages, and any program of the user can
// open the pipe. Parsing validates every field and rejects the whole message on any doubt.
namespace ldm::browser {

inline constexpr int kProtocolVersion = 1;
// Browser to LDM: big enough for a page's link list or a form's POST body.
inline constexpr std::uint32_t kMaxInboundBytes = 4 * 1024 * 1024;
// LDM to browser: Chrome's limit for messages from a native host.
inline constexpr std::uint32_t kMaxOutboundBytes = 1024 * 1024;
// A form body handed over with a capture, after base64 decoding.
inline constexpr size_t kMaxPostBytes = 1024 * 1024;
// Links of one page handed over at once, and the cookies that come with them.
inline constexpr size_t kMaxPageLinks = 5000;
inline constexpr size_t kMaxPageLinkCookies = 1000;
// Bytes of UTF-8 in one link text.
inline constexpr size_t kMaxLinkText = 1024;

enum class BrowserFamily {
    Chromium,  // Chrome and Edge: same extension, same host manifest format
    Firefox,
};

// Which extension started this host. ldm-host.exe sends it first, read from the arguments the
// browser started it with; the browser only starts the host for extensions its manifest allows.
struct HostInfo {
    BrowserFamily family = BrowserFamily::Chromium;
    std::string extensionId;
};

// The extension's first message on every connection.
struct Hello {
    std::string browser;  // display name, such as "Chrome" or "Firefox"
    std::string browserVersion;
    std::string extensionVersion;
    bool incognitoAllowed = false;
    // Whether ldm-host.exe should start LDM when it is not running. The extension asks for it only
    // when it has work for LDM; a plain reconnect must not undo the user's Exit.
    bool startLdm = false;
};

// The form submission a captured download came from.
struct PostBody {
    std::string contentType;
    std::string body;  // bytes
};

// A download the extension took over from the browser (M3 design section 5). The browser's own
// download is paused until LDM answers with a capture-result.
struct Capture {
    std::string url;  // http, https, ftp or ftps
    std::optional<std::string> finalUrl;  // after the browser followed redirects
    std::optional<std::string> referrer;  // the page the download started from
    std::optional<std::string> fileName;  // the name the browser chose, possibly from Content-Disposition
    std::optional<std::int64_t> fileSize;
    std::optional<std::string> mimeType;
    std::string userAgent;
    std::vector<model::Cookie> cookies;  // what the browser would send to url and finalUrl
    std::optional<PostBody> post;        // set when the download answered a form submission
};

// Where a link of a page was found, as the reference tells them apart.
enum class LinkKind {
    Link,   // <a> and <area>
    Image,  // a picture shown on the page (<img>)
    Frame,  // the page of an <iframe>
    Text,   // an address written in the page's text or scripts
};

struct PageLink {
    std::string url;  // http, https, ftp or ftps
    std::string text;  // the link's text or the picture's alt text, UTF-8; may be empty
    std::optional<std::string> fileName;  // the link's download attribute
    LinkKind kind = LinkKind::Link;
};

// "Download all links" or "Download selected links" from the browser's right-click menu
// (checklists B-04, B-05). Opens the Download All Links dialog.
struct Links {
    bool selection = false;  // only what the user selected on the page
    std::optional<std::string> referrer;  // the page the links are on
    std::string userAgent;
    std::vector<PageLink> links;  // at least one
    // What the browser would send to the links, for all of them together; each download keeps
    // only the cookies that belong to its own address.
    std::vector<model::Cookie> cookies;
};

// What the extension found out about some links of a Links message, sent on the same connection
// while the Download All Links dialog is open.
struct LinkDetails {
    size_t index = 0;  // into Links::links; the receiver checks the range
    bool failed = false;  // the request failed; nothing else is set
    std::optional<std::int64_t> fileSize;
    std::optional<std::string> mimeType;
    std::optional<std::string> fileName;  // from Content-Disposition, not yet sanitized
};

struct LinkInfo {
    std::vector<LinkDetails> items;
};

using Message = std::variant<HostInfo, Hello, Capture, Links, LinkInfo>;

struct ParsedMessage {
    std::string id;  // echoed in answers
    Message message;
};

// The checks ldm-host.exe applies before relaying: a JSON object of the right version with an id
// and a well-formed type. The type's own fields are LDM's to validate.
[[nodiscard]] bool IsValidEnvelope(std::string_view json);

// For ldm-host.exe: true when `json` is a hello that asks for LDM to be started.
[[nodiscard]] bool HelloAsksToStartLdm(std::string_view json);

// Nothing when the message is malformed, of an unknown type, or has an invalid field.
[[nodiscard]] std::optional<ParsedMessage> ParseMessage(std::string_view json);

// Reads the arguments a browser starts a native host with: Chrome and Edge pass the caller's
// origin ("chrome-extension://<id>/"), Firefox passes the manifest path and the add-on id.
[[nodiscard]] std::optional<HostInfo> HostInfoFromArguments(const std::vector<std::wstring>& arguments);

[[nodiscard]] std::string EncodeHostInfo(const HostInfo& info);

// Standard base64 (RFC 4648, with padding). Nothing for anything else, including whitespace.
[[nodiscard]] std::optional<std::string> DecodeBase64(std::string_view text);

// Capture rules LDM pushes to the extension; the extension decides locally from them (M3 design
// decision 2). Grows with each capture feature.
struct Config {
    std::string appVersion;
    std::vector<std::string> capturedExtensions;  // lower-case patterns without dots; '*' matches anything
    std::vector<std::string> siteExceptions;      // host or host/path patterns, '*' wildcards
    std::vector<std::string> addressExceptions;   // whole-address patterns, '*' wildcards
    model::CaptureKeys keys;
    model::BrowserMenu menu;  // for this browser's family
};

[[nodiscard]] std::string EncodeConfig(const Config& config, std::string_view replyTo);

// The answer to a capture: accepted means LDM has taken the download over and the extension
// cancels the browser's copy; otherwise the browser continues it.
[[nodiscard]] std::string EncodeCaptureResult(std::string_view replyTo, bool accepted);

// The answer to a Links message. Accepted: the dialog opens, and the extension may send LinkInfo
// on the same connection until LDM sends links-done.
[[nodiscard]] std::string EncodeLinksResult(std::string_view replyTo, bool accepted);
// The Download All Links dialog closed: the extension stops checking the links and disconnects.
[[nodiscard]] std::string EncodeLinksDone(std::string_view replyTo);

// Refresh download address (checklist ENG-08): asks the browser to open the page a download came
// from, so the user can start it again there. `url` is http or https.
[[nodiscard]] std::string EncodeOpenPage(std::string_view url);

}  // namespace ldm::browser
