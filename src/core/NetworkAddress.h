#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

// Which addresses belong to the user's own networks (M3 design section 6): a stream whose
// manifest came from a public host may not make LDM send requests to them, so a hostile manifest
// cannot reach a router or NAS on the home network through LDM.
namespace ldm::core {

using Ipv6Address = std::array<std::uint8_t, 16>;

// Loopback, private (RFC 1918), carrier-grade NAT (100.64.0.0/10, which also covers Tailscale),
// link-local, "this network", benchmarking, multicast and reserved addresses. `address` is in
// host order: 127.0.0.1 is 0x7F000001.
[[nodiscard]] bool IsPrivateIpv4(std::uint32_t address) noexcept;
// Loopback, unspecified, unique local (fc00::/7), link-local, site-local and multicast, and IPv4
// addresses inside IPv6 (mapped, compatible, NAT64 and 6to4) by the IPv4 rule.
[[nodiscard]] bool IsPrivateIpv6(const Ipv6Address& address) noexcept;

// A host as an address names it (lower case, IPv6 without brackets): a private literal address,
// "localhost" and names under it, a name of one label (resolved on the local network), and the
// local-only suffixes .local, .internal and .home.arpa. Other names may still resolve to a
// private address; the engine checks every connection it opens for that.
[[nodiscard]] bool IsPrivateHost(std::string_view host);

// Strict literal forms: four decimal parts for IPv4, RFC 4291 text for IPv6 (an optional zone
// after '%' is ignored).
[[nodiscard]] std::optional<std::uint32_t> ParseIpv4(std::string_view text) noexcept;
[[nodiscard]] std::optional<Ipv6Address> ParseIpv6(std::string_view text);

}  // namespace ldm::core
