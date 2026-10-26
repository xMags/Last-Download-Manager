#include "engine/ConnectionGuards.h"

#include "core/NetworkAddress.h"

#include <string_view>

namespace ldm::engine {

namespace {

bool IsPrivateText(std::string_view address)
{
    if (const auto v4 = core::ParseIpv4(address)) {
        return core::IsPrivateIpv4(*v4);
    }
    if (const auto v6 = core::ParseIpv6(address)) {
        return core::IsPrivateIpv6(*v6);
    }
    return true;  // libcurl always names the address; something else is not trusted
}

int PortOf(const core::Url& url)
{
    if (url.port) {
        return *url.port;
    }
    if (url.scheme == "https") {
        return 443;
    }
    if (url.scheme == "ftp") {
        return 21;
    }
    if (url.scheme == "ftps") {
        return 990;
    }
    return 80;
}

// The same host as libcurl counts it for credentials: scheme, name and port.
bool SameHost(const core::Url& a, const core::Url& b)
{
    return a.scheme == b.scheme && a.host == b.host && PortOf(a) == PortOf(b);
}

int CheckConnection(void* clientp, char* primaryIp, char* localIp, int primaryPort, int localPort)
{
    (void)localIp;
    (void)primaryPort;
    (void)localPort;
    auto* guards = static_cast<ConnectionGuards*>(clientp);
    if (primaryIp != nullptr && IsPrivateText(primaryIp)) {
        guards->address.reachedPrivate = true;
        if (guards->address.refuse) {
            return CURL_PREREQFUNC_ABORT;
        }
    }
    if (!guards->host.first) {
        return CURL_PREREQFUNC_OK;
    }
    // Following a redirect, libcurl already points the handle at the address it is about to ask.
    char* current = nullptr;
    if (curl_easy_getinfo(guards->easy, CURLINFO_EFFECTIVE_URL, &current) != CURLE_OK || current == nullptr) {
        return CURL_PREREQFUNC_ABORT;  // cannot tell where it goes: the headers stay back
    }
    const auto url = core::ParseUrl(current);
    if (url && SameHost(*url, *guards->host.first)) {
        return CURL_PREREQFUNC_OK;
    }
    guards->host.elsewhere = current;
    return CURL_PREREQFUNC_ABORT;
}

}  // namespace

void InstallGuards(CURL* easy, ConnectionGuards* guards)
{
    guards->easy = easy;
    curl_easy_setopt(easy, CURLOPT_PREREQFUNCTION, &CheckConnection);
    curl_easy_setopt(easy, CURLOPT_PREREQDATA, guards);
}

bool StoppedForOtherHost(const ConnectionGuards& guards, CURLcode result) noexcept
{
    return result == CURLE_ABORTED_BY_CALLBACK && !guards.host.elsewhere.empty();
}

bool ResendElsewhere(CURLM* multi, ConnectionGuards& guards, curl_slist* headers)
{
    CURL* easy = guards.easy;
    curl_multi_remove_handle(multi, easy);
    curl_easy_setopt(easy, CURLOPT_URL, guards.host.elsewhere.c_str());  // copied by libcurl
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    // libcurl sends these to the host of the first address only; from here on that would be the
    // other host.
    curl_easy_setopt(easy, CURLOPT_USERNAME, nullptr);
    curl_easy_setopt(easy, CURLOPT_PASSWORD, nullptr);
    guards.host.first.reset();
    guards.host.elsewhere.clear();
    return curl_multi_add_handle(multi, easy) == CURLM_OK;
}

}  // namespace ldm::engine
