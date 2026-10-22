#pragma once

#include "core/Ports.h"

#include <memory>
#include <optional>
#include <string>

namespace ldm::platform {

// Options > Proxy (checklist O-06). libcurl cannot run PAC scripts, so "system" and "automatic
// configuration script" modes ask WinHTTP for the proxy of each address and hand the answer to
// libcurl (design section 4). Manual mode builds the proxy URL from the settings, including
// DPAPI-protected credentials and the bypass list.
//
// WinHTTP may fetch a PAC script or run WPAD detection on the first call, which can take a few
// seconds; it caches the result afterwards.
class WinHttpProxyResolver final : public core::ProxyResolver {
public:
    explicit WinHttpProxyResolver(core::SecretProtector& secrets);
    ~WinHttpProxyResolver() override;
    WinHttpProxyResolver(const WinHttpProxyResolver&) = delete;
    WinHttpProxyResolver& operator=(const WinHttpProxyResolver&) = delete;

    void Configure(const model::Settings& settings) override;
    std::optional<std::string> ProxyFor(const std::string& url) override;

private:
    [[nodiscard]] std::optional<std::string> ResolveWithWinHttp(const std::string& url,
                                                                const std::optional<std::string>& scriptUrl);
    [[nodiscard]] std::optional<std::string> ManualProxy(const std::string& url) const;
    [[nodiscard]] bool Bypassed(const std::string& host) const;

    core::SecretProtector& secrets_;
    model::Settings settings_;
    void* session_ = nullptr;  // HINTERNET, opened on first use
};

}  // namespace ldm::platform
