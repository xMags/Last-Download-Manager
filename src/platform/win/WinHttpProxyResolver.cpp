#include "platform/win/WinHttpProxyResolver.h"

#include "base/Utf.h"
#include "core/Matching.h"
#include "core/Url.h"

#include <windows.h>
#include <winhttp.h>

#include <string_view>
#include <vector>

namespace ldm::platform {

namespace {

// Frees strings WinHTTP allocates with GlobalAlloc.
class GlobalString {
public:
    GlobalString() = default;
    GlobalString(const GlobalString&) = delete;
    GlobalString& operator=(const GlobalString&) = delete;
    ~GlobalString()
    {
        if (value != nullptr) {
            ::GlobalFree(value);
        }
    }
    LPWSTR value = nullptr;
};

std::string PercentEncode(std::string_view text)
{
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string result;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                                c == '.' || c == '_' || c == '~';
        if (unreserved) {
            result.push_back(c);
        } else {
            result.push_back('%');
            result.push_back(kHex[byte >> 4]);
            result.push_back(kHex[byte & 0x0F]);
        }
    }
    return result;
}

// Picks the entry for `scheme` from a WinHTTP proxy list: "host:port", "host:port;other:port" or
// "http=host:port;https=host2:port". Returns nothing for DIRECT or an empty list.
std::optional<std::string> PickFromList(std::string_view list, std::string_view scheme)
{
    std::optional<std::string> generic;
    size_t pos = 0;
    while (pos < list.size()) {
        size_t end = list.find_first_of("; ", pos);
        if (end == std::string_view::npos) {
            end = list.size();
        }
        std::string_view entry = list.substr(pos, end - pos);
        pos = end + 1;
        if (entry.empty()) {
            continue;
        }
        if (entry == "DIRECT") {
            if (!generic) {
                return std::nullopt;
            }
            continue;
        }
        const size_t equals = entry.find('=');
        if (equals == std::string_view::npos) {
            if (!generic) {
                generic = std::string(entry);
            }
            continue;
        }
        if (entry.substr(0, equals) == scheme) {
            return std::string(entry.substr(equals + 1));
        }
    }
    return generic;
}

std::string WithScheme(const std::string& proxy)
{
    return proxy.find("://") == std::string::npos ? "http://" + proxy : proxy;
}

}  // namespace

WinHttpProxyResolver::WinHttpProxyResolver(core::SecretProtector& secrets) : secrets_(secrets) {}

WinHttpProxyResolver::~WinHttpProxyResolver()
{
    if (session_ != nullptr) {
        ::WinHttpCloseHandle(session_);
    }
}

void WinHttpProxyResolver::Configure(const model::Settings& settings)
{
    settings_ = settings;
}

std::optional<std::string> WinHttpProxyResolver::ProxyFor(const std::string& url)
{
    switch (settings_.proxyMode) {
    case model::ProxyMode::None:
        return std::nullopt;
    case model::ProxyMode::System:
        return ResolveWithWinHttp(url, std::nullopt);
    case model::ProxyMode::AutoConfigScript:
        if (settings_.autoConfigScriptUrl.empty()) {
            return std::nullopt;
        }
        return ResolveWithWinHttp(url, settings_.autoConfigScriptUrl);
    case model::ProxyMode::Manual:
        return ManualProxy(url);
    }
    return std::nullopt;
}

std::optional<std::string> WinHttpProxyResolver::ResolveWithWinHttp(const std::string& url,
                                                                    const std::optional<std::string>& scriptUrl)
{
    const auto parsed = core::ParseUrl(url);
    if (!parsed) {
        return std::nullopt;
    }
    std::optional<std::string> script = scriptUrl;
    bool autoDetect = false;
    std::optional<std::string> staticProxy;
    std::string bypass;
    if (!script) {
        WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config{};
        if (!::WinHttpGetIEProxyConfigForCurrentUser(&config)) {
            return std::nullopt;
        }
        GlobalString autoConfig;
        GlobalString proxy;
        GlobalString proxyBypass;
        autoConfig.value = config.lpszAutoConfigUrl;
        proxy.value = config.lpszProxy;
        proxyBypass.value = config.lpszProxyBypass;
        autoDetect = config.fAutoDetect != FALSE;
        if (autoConfig.value != nullptr) {
            script = base::ToUtf8(autoConfig.value);
        }
        if (proxy.value != nullptr) {
            staticProxy = base::ToUtf8(proxy.value);
        }
        if (proxyBypass.value != nullptr) {
            bypass = base::ToUtf8(proxyBypass.value);
        }
    }

    if (script || autoDetect) {
        if (session_ == nullptr) {
            session_ = ::WinHttpOpen(L"Last Download Manager", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
        }
        if (session_ != nullptr) {
            WINHTTP_AUTOPROXY_OPTIONS options{};
            std::wstring scriptWide;
            if (script) {
                scriptWide = base::ToUtf16(*script);
                options.dwFlags = WINHTTP_AUTOPROXY_CONFIG_URL;
                options.lpszAutoConfigUrl = scriptWide.c_str();
            } else {
                options.dwFlags = WINHTTP_AUTOPROXY_AUTO_DETECT;
                options.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
            }
            options.fAutoLogonIfChallenged = TRUE;
            WINHTTP_PROXY_INFO info{};
            if (::WinHttpGetProxyForUrl(session_, base::ToUtf16(url).c_str(), &options, &info)) {
                GlobalString proxy;
                GlobalString infoBypass;
                proxy.value = info.lpszProxy;
                infoBypass.value = info.lpszProxyBypass;
                if (info.dwAccessType != WINHTTP_ACCESS_TYPE_NAMED_PROXY || proxy.value == nullptr) {
                    return std::nullopt;
                }
                const auto picked = PickFromList(base::ToUtf8(proxy.value), parsed->scheme);
                return picked ? std::optional<std::string>(WithScheme(*picked)) : std::nullopt;
            }
        }
        // Detection or the script failed: fall through to the static proxy, as browsers do.
    }
    if (!staticProxy) {
        return std::nullopt;
    }
    size_t pos = 0;
    while (pos < bypass.size()) {
        size_t end = bypass.find_first_of("; ", pos);
        if (end == std::string::npos) {
            end = bypass.size();
        }
        const std::string pattern = bypass.substr(pos, end - pos);
        pos = end + 1;
        if (pattern == "<local>" ? parsed->host.find('.') == std::string::npos : core::WildcardMatch(pattern, parsed->host)) {
            return std::nullopt;
        }
    }
    const auto picked = PickFromList(*staticProxy, parsed->scheme);
    return picked ? std::optional<std::string>(WithScheme(*picked)) : std::nullopt;
}

bool WinHttpProxyResolver::Bypassed(const std::string& host) const
{
    const std::string& list = settings_.proxyBypass;
    size_t pos = 0;
    while (pos < list.size()) {
        size_t end = list.find_first_of(" ;,", pos);
        if (end == std::string::npos) {
            end = list.size();
        }
        const std::string pattern = list.substr(pos, end - pos);
        pos = end + 1;
        if (pattern.empty()) {
            continue;
        }
        if (pattern == "<local>" ? host.find('.') == std::string::npos : core::WildcardMatch(pattern, host)) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> WinHttpProxyResolver::ManualProxy(const std::string& url) const
{
    const auto parsed = core::ParseUrl(url);
    if (!parsed || Bypassed(parsed->host)) {
        return std::nullopt;
    }
    const auto credentials = [this](const std::string& user, const std::vector<std::uint8_t>& secret) -> std::string {
        if (user.empty()) {
            return {};
        }
        std::string text = PercentEncode(user);
        if (!secret.empty()) {
            if (auto password = secrets_.Unprotect(secret)) {
                text += ":" + PercentEncode(password.Value());
            }
        }
        return text + "@";
    };
    const model::ProxyServer* server = nullptr;
    if (parsed->scheme == "http") {
        server = &settings_.httpProxy;
    } else if (parsed->scheme == "https") {
        server = &settings_.httpsProxy;
    } else {
        server = &settings_.ftpProxy;
    }
    if (server->enabled && !server->host.empty() && server->port > 0) {
        return "http://" + credentials(server->user, server->secret) + server->host + ":" + std::to_string(server->port);
    }
    const model::SocksServer& socks = settings_.socks;
    if (socks.enabled && !socks.host.empty() && socks.port > 0) {
        // The "h"/"a" variants let the proxy resolve names (proxy DNS).
        const std::string scheme = socks.version == 4 ? (socks.remoteDns ? "socks4a" : "socks4")
                                                      : (socks.remoteDns ? "socks5h" : "socks5");
        return scheme + "://" + credentials(socks.user, socks.secret) + socks.host + ":" + std::to_string(socks.port);
    }
    return std::nullopt;
}

}  // namespace ldm::platform
