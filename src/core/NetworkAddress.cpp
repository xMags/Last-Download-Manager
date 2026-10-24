#include "core/NetworkAddress.h"

#include <cstddef>
#include <vector>

namespace ldm::core {

namespace {

struct Ipv4Range {
    std::uint32_t base;
    int prefix;
};

constexpr Ipv4Range kPrivateIpv4[] = {
    {0x00000000, 8},   // "this network"
    {0x0A000000, 8},   // private
    {0x64400000, 10},  // carrier-grade NAT, Tailscale
    {0x7F000000, 8},   // loopback
    {0xA9FE0000, 16},  // link-local
    {0xAC100000, 12},  // private
    {0xC0000000, 24},  // IETF protocol assignments
    {0xC0A80000, 16},  // private
    {0xC6120000, 15},  // benchmarking
    {0xE0000000, 4},   // multicast
    {0xF0000000, 4},   // reserved, broadcast
};

bool InRange(std::uint32_t address, Ipv4Range range) noexcept
{
    const std::uint32_t mask = range.prefix == 0 ? 0 : ~std::uint32_t{0} << (32 - range.prefix);
    return (address & mask) == range.base;
}

std::uint32_t Ipv4At(const Ipv6Address& address, size_t offset) noexcept
{
    return (std::uint32_t{address[offset]} << 24) | (std::uint32_t{address[offset + 1]} << 16) |
           (std::uint32_t{address[offset + 2]} << 8) | std::uint32_t{address[offset + 3]};
}

bool AllZero(const Ipv6Address& address, size_t from, size_t to) noexcept
{
    for (size_t i = from; i < to; ++i) {
        if (address[i] != 0) {
            return false;
        }
    }
    return true;
}

std::optional<std::uint16_t> ParseHexGroup(std::string_view text) noexcept
{
    if (text.empty() || text.size() > 4) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (const char c : text) {
        value <<= 4;
        if (c >= '0' && c <= '9') {
            value |= static_cast<std::uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value |= static_cast<std::uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            value |= static_cast<std::uint32_t>(c - 'A' + 10);
        } else {
            return std::nullopt;
        }
    }
    return static_cast<std::uint16_t>(value);
}

// The 16-bit groups of one side of "::"; an IPv4 tail counts as two groups when allowed.
std::optional<std::vector<std::uint16_t>> ParseGroups(std::string_view text, bool allowIpv4Tail)
{
    std::vector<std::uint16_t> groups;
    if (text.empty()) {
        return groups;
    }
    size_t start = 0;
    while (true) {
        const size_t colon = text.find(':', start);
        const std::string_view part = text.substr(start, colon == std::string_view::npos ? text.size() - start : colon - start);
        if (colon == std::string_view::npos && allowIpv4Tail && part.find('.') != std::string_view::npos) {
            const auto v4 = ParseIpv4(part);
            if (!v4) {
                return std::nullopt;
            }
            groups.push_back(static_cast<std::uint16_t>(*v4 >> 16));
            groups.push_back(static_cast<std::uint16_t>(*v4 & 0xFFFF));
            return groups;
        }
        const auto group = ParseHexGroup(part);
        if (!group) {
            return std::nullopt;
        }
        groups.push_back(*group);
        if (colon == std::string_view::npos) {
            return groups;
        }
        start = colon + 1;
    }
}

bool EndsWith(std::string_view text, std::string_view suffix) noexcept
{
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

}  // namespace

bool IsPrivateIpv4(std::uint32_t address) noexcept
{
    for (const Ipv4Range range : kPrivateIpv4) {
        if (InRange(address, range)) {
            return true;
        }
    }
    return false;
}

bool IsPrivateIpv6(const Ipv6Address& address) noexcept
{
    // ::/96 holds :: and ::1 and the old IPv4-compatible form; ::ffff:0:0/96 is IPv4-mapped.
    if (AllZero(address, 0, 10)) {
        const bool compatible = address[10] == 0 && address[11] == 0;
        const bool mapped = address[10] == 0xFF && address[11] == 0xFF;
        if (compatible) {
            const std::uint32_t tail = Ipv4At(address, 12);
            return tail <= 1 || IsPrivateIpv4(tail);
        }
        if (mapped) {
            return IsPrivateIpv4(Ipv4At(address, 12));
        }
    }
    if ((address[0] & 0xFE) == 0xFC) {
        return true;  // unique local
    }
    if (address[0] == 0xFE && (address[1] & 0xC0) == 0x80) {
        return true;  // link-local
    }
    if (address[0] == 0xFE && (address[1] & 0xC0) == 0xC0) {
        return true;  // site-local (deprecated, still routed nowhere public)
    }
    if (address[0] == 0xFF) {
        return true;  // multicast
    }
    // 64:ff9b::/96, NAT64: the IPv4 address it reaches is in the last four bytes.
    if (address[0] == 0x00 && address[1] == 0x64 && address[2] == 0xFF && address[3] == 0x9B && AllZero(address, 4, 12)) {
        return IsPrivateIpv4(Ipv4At(address, 12));
    }
    // 2002::/16, 6to4: the IPv4 address follows the prefix.
    if (address[0] == 0x20 && address[1] == 0x02) {
        return IsPrivateIpv4(Ipv4At(address, 2));
    }
    return false;
}

std::optional<std::uint32_t> ParseIpv4(std::string_view text) noexcept
{
    std::uint32_t address = 0;
    int parts = 0;
    size_t start = 0;
    bool ended = false;  // the last part ran to the end of the text
    while (parts < 4) {
        const size_t dot = text.find('.', start);
        const std::string_view part = text.substr(start, dot == std::string_view::npos ? text.size() - start : dot - start);
        if (part.empty() || part.size() > 3 || (part.size() > 1 && part.front() == '0')) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (const char c : part) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            value = value * 10 + static_cast<std::uint32_t>(c - '0');
        }
        if (value > 255) {
            return std::nullopt;
        }
        address = (address << 8) | value;
        ++parts;
        if (dot == std::string_view::npos) {
            ended = true;
            break;
        }
        start = dot + 1;
    }
    if (parts != 4 || !ended) {
        return std::nullopt;
    }
    return address;
}

std::optional<Ipv6Address> ParseIpv6(std::string_view text)
{
    if (const size_t zone = text.find('%'); zone != std::string_view::npos) {
        text = text.substr(0, zone);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::vector<std::uint16_t> head;
    std::vector<std::uint16_t> tail;
    const size_t gap = text.find("::");
    if (gap != std::string_view::npos) {
        if (text.find("::", gap + 1) != std::string_view::npos) {
            return std::nullopt;
        }
        auto left = ParseGroups(text.substr(0, gap), false);
        auto right = ParseGroups(text.substr(gap + 2), true);
        if (!left || !right || left->size() + right->size() > 7) {
            return std::nullopt;
        }
        head = std::move(*left);
        tail = std::move(*right);
    } else {
        auto all = ParseGroups(text, true);
        if (!all || all->size() != 8) {
            return std::nullopt;
        }
        head = std::move(*all);
    }
    std::vector<std::uint16_t> groups = head;
    groups.resize(8 - tail.size(), 0);
    groups.insert(groups.end(), tail.begin(), tail.end());
    Ipv6Address address{};
    for (size_t i = 0; i < 8; ++i) {
        address[i * 2] = static_cast<std::uint8_t>(groups[i] >> 8);
        address[i * 2 + 1] = static_cast<std::uint8_t>(groups[i] & 0xFF);
    }
    return address;
}

bool IsPrivateHost(std::string_view host)
{
    if (host.empty()) {
        return true;
    }
    if (host.back() == '.') {
        host.remove_suffix(1);  // "localhost." is localhost
    }
    if (host.find(':') != std::string_view::npos) {
        const auto v6 = ParseIpv6(host);
        return !v6 || IsPrivateIpv6(*v6);  // not an address LDM can read: refuse rather than guess
    }
    if (const auto v4 = ParseIpv4(host)) {
        return IsPrivateIpv4(*v4);
    }
    if (host.find('.') == std::string_view::npos) {
        return true;  // "localhost", "nas", "router", and numbers some resolvers read as addresses
    }
    return host == "localhost" || EndsWith(host, ".localhost") || EndsWith(host, ".local") ||
           EndsWith(host, ".internal") || EndsWith(host, ".home.arpa");
}

}  // namespace ldm::core
