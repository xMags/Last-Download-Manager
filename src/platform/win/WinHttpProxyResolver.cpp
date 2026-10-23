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

// Bounds a PAC download or a WPAD probe, so a stuck network never holds a download for long.
constexpr int kLookupTimeoutMs = 10'000;

// The system's static proxy for `url`, honouring its bypass list.
std::optional<std::string> StaticProxyFor(const core::Url& url, const std::optional<std::string>& staticProxy,
                                          const std::string& bypass)
{
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
        if (pattern == "<local>" ? url.host.find('.') == std::string::npos : core::WildcardMatch(pattern, url.host)) {
            return std::nullopt;
        }
    }
    const auto picked = PickFromList(*staticProxy, url.scheme);
    return picked ? std::optional<std::string>(WithScheme(*picked)) : std::nullopt;
}

}  // namespace

WinHttpProxyResolver::WinHttpProxyResolver(core::SecretProtector& secrets, core::EngineListener& listener)
    : secrets_(secrets), listener_(listener)
{
}

WinHttpProxyResolver::~WinHttpProxyResolver()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        jobs_.clear();
    }
    wake_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (session_ != nullptr) {
        ::WinHttpCloseHandle(session_);
    }
}

void WinHttpProxyResolver::Configure(const model::Settings& settings)
{
    settings_ = settings;
}

core::ProxyLookup WinHttpProxyResolver::Resolve(model::DownloadId id, std::int64_t epoch, const std::string& url)
{
    const auto parsed = core::ParseUrl(url);
    if (!parsed) {
        return {};
    }
    Job job;
    job.id = id;
    job.epoch = epoch;
    job.url = url;
    switch (settings_.proxyMode) {
    case model::ProxyMode::None:
        return {};
    case model::ProxyMode::Manual:
        return core::ProxyLookup{false, ManualProxy(url)};
    case model::ProxyMode::AutoConfigScript:
        if (settings_.autoConfigScriptUrl.empty()) {
            return {};
        }
        job.script = settings_.autoConfigScriptUrl;
        break;
    case model::ProxyMode::System: {
        // Reading the system settings is a quick registry read; only detection and scripts are slow.
        WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config{};
        if (!::WinHttpGetIEProxyConfigForCurrentUser(&config)) {
            return {};
        }
        GlobalString autoConfig;
        GlobalString proxy;
        GlobalString proxyBypass;
        autoConfig.value = config.lpszAutoConfigUrl;
        proxy.value = config.lpszProxy;
        proxyBypass.value = config.lpszProxyBypass;
        job.autoDetect = config.fAutoDetect != FALSE;
        if (autoConfig.value != nullptr) {
            job.script = base::ToUtf8(autoConfig.value);
        }
        if (proxy.value != nullptr) {
            job.staticProxy = base::ToUtf8(proxy.value);
        }
        if (proxyBypass.value != nullptr) {
            job.bypass = base::ToUtf8(proxyBypass.value);
        }
        if (!job.script && !job.autoDetect) {
            return core::ProxyLookup{false, StaticProxyFor(*parsed, job.staticProxy, job.bypass)};
        }
        break;
    }
    }
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            return {};
        }
        jobs_.push_back(std::move(job));
        if (!worker_.joinable()) {
            worker_ = std::thread([this] { RunWorker(); });
        }
    }
    wake_.notify_one();
    return core::ProxyLookup{true, std::nullopt};
}

void WinHttpProxyResolver::RunWorker()
{
    while (true) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_) {
                return;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        std::optional<std::string> proxy = LookUp(job);
        listener_.OnEngineEvent(core::EngineEvent{job.id, job.epoch, core::ProxyResolved{std::move(proxy)}});
    }
}

std::optional<std::string> WinHttpProxyResolver::LookUp(const Job& job)
{
    const auto parsed = core::ParseUrl(job.url);
    if (!parsed) {
        return std::nullopt;
    }
    if (job.script || job.autoDetect) {
        if (session_ == nullptr) {
            session_ = ::WinHttpOpen(L"Last Download Manager", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
            if (session_ != nullptr) {
                ::WinHttpSetTimeouts(session_, kLookupTimeoutMs, kLookupTimeoutMs, kLookupTimeoutMs, kLookupTimeoutMs);
            }
        }
        if (session_ != nullptr) {
            WINHTTP_AUTOPROXY_OPTIONS options{};
            std::wstring scriptWide;
            if (job.script) {
                scriptWide = base::ToUtf16(*job.script);
                options.dwFlags = WINHTTP_AUTOPROXY_CONFIG_URL;
                options.lpszAutoConfigUrl = scriptWide.c_str();
            } else {
                options.dwFlags = WINHTTP_AUTOPROXY_AUTO_DETECT;
                options.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
            }
            options.fAutoLogonIfChallenged = TRUE;
            WINHTTP_PROXY_INFO info{};
            if (::WinHttpGetProxyForUrl(session_, base::ToUtf16(job.url).c_str(), &options, &info)) {
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
    return StaticProxyFor(*parsed, job.staticProxy, job.bypass);
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
