#include "model/Settings.h"

#include <gtest/gtest.h>

namespace {

using ldm::model::DuplicatePolicy;
using ldm::model::ProxyMode;
using ldm::model::Settings;
using ldm::model::SettingsFromMap;
using ldm::model::SettingsMap;
using ldm::model::SettingsToMap;

TEST(Settings, EmptyMapGivesDefaults)
{
    const auto loaded = SettingsFromMap({});
    EXPECT_TRUE(loaded.warnings.empty());
    EXPECT_EQ(loaded.settings.defaultMaxConnections, 8);
    EXPECT_EQ(loaded.settings.userAgent, ldm::model::DefaultUserAgent());
    EXPECT_EQ(loaded.settings.duplicatePolicy, DuplicatePolicy::Ask);
}

TEST(Settings, RoundTripsEveryChangedField)
{
    Settings original;
    original.monitorClipboard = false;
    original.defaultMaxConnections = 16;
    original.duplicatePolicy = DuplicatePolicy::Overwrite;
    original.proxyMode = ProxyMode::Manual;
    original.httpProxy.enabled = true;
    original.httpProxy.host = "proxy.example";
    original.httpProxy.port = 3128;
    original.httpProxy.secret = {0x00, 0x7F, 0xFF};
    original.socks.version = 4;
    original.temporaryDirectory = "D:\\\xE2\x82\xAC temp";
    original.mediaPanelEnabled = false;
    original.mediaTypes = "MP4 WEBM";
    original.mediaMinimumKilobytes = 0;
    original.mediaSiteExceptions = "*.example.test";
    original.mediaDrmNotice = false;
    original.siteVideosEnabled = true;
    original.siteVideosYtDlpCheckUnixMs = 1'792'909'519'000;
    original.siteVideosDenoCheckUnixMs = 1'792'000'000'000;

    const auto loaded = SettingsFromMap(SettingsToMap(original));
    ASSERT_TRUE(loaded.warnings.empty());
    EXPECT_EQ(SettingsToMap(loaded.settings), SettingsToMap(original));
    EXPECT_EQ(loaded.settings.httpProxy.secret, original.httpProxy.secret);
}

TEST(Settings, InvalidValuesFallBackToDefaultsWithWarnings)
{
    const SettingsMap stored{
        {"connection.defaultMaxConnections", "99"},
        {"general.monitorClipboard", "yes"},
        {"downloads.duplicatePolicy", "explode"},
        {"proxy.http.secret", "abc"},
        {"proxy.http.port", "8080x"},
    };
    const auto loaded = SettingsFromMap(stored);
    EXPECT_EQ(loaded.warnings.size(), 5u);
    const Settings defaults;
    EXPECT_EQ(loaded.settings.defaultMaxConnections, defaults.defaultMaxConnections);
    EXPECT_EQ(loaded.settings.monitorClipboard, defaults.monitorClipboard);
    EXPECT_EQ(loaded.settings.duplicatePolicy, defaults.duplicatePolicy);
    EXPECT_TRUE(loaded.settings.httpProxy.secret.empty());
    EXPECT_EQ(loaded.settings.httpProxy.port, defaults.httpProxy.port);
}

TEST(Settings, UnknownKeysAreIgnored)
{
    const auto loaded = SettingsFromMap({{"future.setting", "1"}});
    EXPECT_TRUE(loaded.warnings.empty());
}

TEST(Settings, MapContainsEveryKey)
{
    const SettingsMap map = SettingsToMap(Settings{});
    EXPECT_TRUE(map.contains("proxy.socks.remoteDns"));
    EXPECT_TRUE(map.contains("confirm.deleteIncomplete"));
    EXPECT_EQ(map.at("siteVideos.enabled"), "0") << "off until the user turns it on";
    EXPECT_GT(map.size(), 60u);
}

}  // namespace
