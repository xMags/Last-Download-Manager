#pragma once

#include "core/Url.h"

#include <curl/curl.h>

#include <optional>
#include <string>

namespace ldm::engine {

// Records what a connection reached, for the private-address rule of streams (M3 design
// section 6).
struct AddressGuard {
    bool refuse = false;           // refuse loopback and private addresses
    bool reachedPrivate = false;   // a connection went to one (refused when `refuse`)
};

// Keeps headers meant for one host (a site video's own headers: stream keys, session ids) from
// reaching another host through a redirect. libcurl follows redirects itself and sends custom
// headers on to every host; it holds back only Authorization and Cookie. So a request that carries
// such headers stops just before it would be sent to another host, and its owner sends it there
// again without them (ResendElsewhere).
struct HostGuard {
    // The host the headers are for (scheme, name and port); nothing when the request carries none.
    std::optional<core::Url> first;
    std::string elsewhere;  // the address of the other host, once the request stopped for it
};

// The checks of one easy handle. Owned by the connection; must outlive its easy handle's transfers.
struct ConnectionGuards {
    AddressGuard address;
    HostGuard host;
    CURL* easy = nullptr;  // set by InstallGuards
};

// Checks every request libcurl sends for `easy`, after it connects and before the request goes
// out, so a public name that resolves to a private address and a redirect to one are covered too,
// as is each redirect to another host. Without `guards->address.refuse` the address check only
// records what was reached; without `guards->host.first` the host check does nothing.
void InstallGuards(CURL* easy, ConnectionGuards* guards);

// True when the transfer ended with `result` because its request stopped before another host.
[[nodiscard]] bool StoppedForOtherHost(const ConnectionGuards& guards, CURLcode result) noexcept;

// Sends a request that stopped before another host on to it: `headers` replace the request's
// headers (the ones without those for the first host, or null), the user name and password are
// dropped as libcurl drops them for another host, and the handle goes into `multi` again. The
// host check is off from then on. False when the handle could not be added again.
[[nodiscard]] bool ResendElsewhere(CURLM* multi, ConnectionGuards& guards, curl_slist* headers);

}  // namespace ldm::engine
