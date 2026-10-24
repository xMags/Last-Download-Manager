#include "core/NetworkAddress.h"

#include <gtest/gtest.h>

namespace {

using ldm::core::IsPrivateHost;
using ldm::core::ParseIpv4;
using ldm::core::ParseIpv6;

TEST(NetworkAddress, ReadsOnlyStrictIpv4)
{
    EXPECT_EQ(ParseIpv4("127.0.0.1"), 0x7F000001u);
    EXPECT_EQ(ParseIpv4("255.255.255.255"), 0xFFFFFFFFu);
    EXPECT_FALSE(ParseIpv4("1.2.3"));
    EXPECT_FALSE(ParseIpv4("1.2.3.4.5"));
    EXPECT_FALSE(ParseIpv4("1.2.3.4."));
    EXPECT_FALSE(ParseIpv4("01.2.3.4"));
    EXPECT_FALSE(ParseIpv4("256.1.1.1"));
    EXPECT_FALSE(ParseIpv4("0x7f.0.0.1"));
    EXPECT_FALSE(ParseIpv4("2130706433"));
}

TEST(NetworkAddress, ReadsIpv6Forms)
{
    const auto loopback = ParseIpv6("::1");
    ASSERT_TRUE(loopback);
    EXPECT_EQ((*loopback)[15], 1);
    const auto mapped = ParseIpv6("::ffff:192.168.1.2");
    ASSERT_TRUE(mapped);
    EXPECT_EQ((*mapped)[10], 0xFF);
    EXPECT_EQ((*mapped)[12], 192);
    EXPECT_TRUE(ParseIpv6("2001:db8:0:0:0:0:0:1"));
    EXPECT_TRUE(ParseIpv6("fe80::1%25eth0"));
    EXPECT_FALSE(ParseIpv6("1::2::3"));
    EXPECT_FALSE(ParseIpv6("1:2:3:4:5:6:7"));
    EXPECT_FALSE(ParseIpv6("12345::"));
    EXPECT_FALSE(ParseIpv6(":"));
}

TEST(NetworkAddress, PrivateHostsAreTheUsersOwnNetworks)
{
    for (const char* host : {"localhost", "localhost.", "app.localhost", "nas", "router.local", "printer.home.arpa",
                             "db.internal", "127.0.0.1", "127.8.8.8", "10.1.2.3", "172.16.0.1", "172.31.255.255",
                             "192.168.0.10", "169.254.1.1", "100.86.196.24", "0.0.0.0", "224.0.0.1",
                             "255.255.255.255", "::1", "::", "fc00::1", "fd12:3456::1", "fe80::1",
                             "::ffff:10.0.0.1", "::ffff:127.0.0.1", "64:ff9b::a00:1", "2002:c0a8:0101::1",
                             "ff02::1", "1:2", ""}) {
        EXPECT_TRUE(IsPrivateHost(host)) << host;
    }
}

TEST(NetworkAddress, PublicHostsAreNot)
{
    for (const char* host : {"example.com", "cdn.example.com", "8.8.8.8", "172.32.0.1", "100.63.255.255",
                             "100.128.0.1", "192.169.0.1", "2606:4700::1111", "::ffff:8.8.8.8", "64:ff9b::808:808",
                             "2002:0808:0808::1"}) {
        EXPECT_FALSE(IsPrivateHost(host)) << host;
    }
}

}  // namespace
