#pragma once

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

using Message = std::variant<HostInfo, Hello>;

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

// Capture rules LDM pushes to the extension; the extension decides locally from them (M3 design
// decision 2). Grows with each capture feature.
struct Config {
    std::string appVersion;
    std::vector<std::string> capturedExtensions;  // lower-case patterns without dots; '*' matches anything
};

[[nodiscard]] std::string EncodeConfig(const Config& config, std::string_view replyTo);

}  // namespace ldm::browser
