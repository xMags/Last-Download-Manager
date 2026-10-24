#pragma once

#include <curl/curl.h>

namespace ldm::engine {

// Records what a connection reached, for the private-address rule of streams (M3 design
// section 6). Owned by the connection; must outlive its easy handle's transfers.
struct AddressGuard {
    bool refuse = false;           // refuse loopback and private addresses
    bool reachedPrivate = false;   // a connection went to one (refused when `refuse`)
};

// Checks every connection libcurl makes for `easy`, after it connects and before the request is
// sent, so a public name that resolves to a private address and a redirect to one are covered
// too. Without `guard->refuse` it only records what was reached.
void InstallAddressGuard(CURL* easy, AddressGuard* guard);

}  // namespace ldm::engine
