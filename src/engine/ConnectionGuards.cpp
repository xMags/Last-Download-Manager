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

int CheckConnection(void* clientp, char* primaryIp, char* localIp, int primaryPort, int localPort)
{
    (void)localIp;
    (void)primaryPort;
    (void)localPort;
    auto* guard = static_cast<AddressGuard*>(clientp);
    if (primaryIp == nullptr || !IsPrivateText(primaryIp)) {
        return CURL_PREREQFUNC_OK;
    }
    guard->reachedPrivate = true;
    return guard->refuse ? CURL_PREREQFUNC_ABORT : CURL_PREREQFUNC_OK;
}

}  // namespace

void InstallAddressGuard(CURL* easy, AddressGuard* guard)
{
    curl_easy_setopt(easy, CURLOPT_PREREQFUNCTION, &CheckConnection);
    curl_easy_setopt(easy, CURLOPT_PREREQDATA, guard);
}

}  // namespace ldm::engine
