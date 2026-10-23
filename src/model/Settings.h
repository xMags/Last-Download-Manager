#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ldm::model {

enum class DuplicatePolicy {
    Ask,
    AddNumbered,
    Overwrite,
    ResumeOrShowComplete,
};

enum class ProgressStartView {
    Normal,
    Minimized,
    Hidden,
};

enum class SpeedProfile {
    Low,
    Medium,
    High,
};

enum class ProxyMode {
    None,
    System,
    AutoConfigScript,
    Manual,
};

enum class TrayIconStyle {
    Color,
    Classic,
    Hidden,
};

enum class DoubleClickAction {
    Open,
    Properties,
};

struct ProxyServer {
    bool enabled = false;
    std::string host;
    int port = 0;
    std::string user;
    std::vector<std::uint8_t> secret;  // DPAPI-protected password
};

struct SocksServer {
    bool enabled = false;
    std::string host;
    int port = 1080;
    std::string user;
    std::vector<std::uint8_t> secret;
    int version = 5;  // 4 or 5
    bool remoteDns = true;
};

struct VirusScanSettings {
    std::string program;    // empty = disabled
    std::string arguments;  // "[File]" is replaced by the downloaded path
};

struct DownloadQuota {
    bool enabled = false;
    int megabytes = 200;
    int hours = 1;
    bool warnBeforeStopping = true;
};

struct SoundEvent {
    bool enabled = false;
    std::string file;
};

// Browser-like User-Agent sent with manually added downloads unless the user sets another.
[[nodiscard]] std::string DefaultUserAgent();

// Extensions captured automatically (checklist O-02 default list), space separated.
[[nodiscard]] std::string DefaultCapturedExtensions();
// Sites whose downloads the browser keeps (checklist O-02), space separated, '*' wildcards.
[[nodiscard]] std::string DefaultCaptureSiteExceptions();

// Every user setting, with its default. Keys, ranges and fallbacks live in Settings.cpp only.
struct Settings {
    // Options > General
    bool launchOnStartup = false;
    bool monitorClipboard = true;

    // Options > File types (checklist O-02, B-02)
    std::string capturedExtensions = DefaultCapturedExtensions();
    std::string captureSiteExceptions = DefaultCaptureSiteExceptions();
    std::string captureAddressExceptions;  // one address pattern per line, '*' wildcards

    // Options > Save to
    std::string temporaryDirectory;  // empty = keep partial files next to the destination
    bool setFileDateFromServer = false;

    // Options > Downloads
    bool showStartDialog = true;
    bool queueOnly = false;
    bool showCompleteDialog = true;
    bool speculativeStart = true;
    bool queuePanelOnLater = true;
    bool queuePanelOnBatch = true;
    bool ignoreModificationTimeChanges = false;
    DuplicatePolicy duplicatePolicy = DuplicatePolicy::Ask;
    std::string userAgent = DefaultUserAgent();
    VirusScanSettings virusScan;

    // Download progress window (checklist P-06)
    bool progressShowSpeedTab = true;
    bool progressShowCompletionTab = true;
    bool progressShowHideTabButtons = true;
    ProgressStartView progressStartView = ProgressStartView::Normal;

    // Options > Connection
    SpeedProfile speedProfile = SpeedProfile::High;
    int defaultMaxConnections = 8;
    DownloadQuota quota;

    // Options > Proxy / Socks
    ProxyMode proxyMode = ProxyMode::System;
    std::string autoConfigScriptUrl;
    ProxyServer httpProxy;
    ProxyServer httpsProxy;
    ProxyServer ftpProxy;
    SocksServer socks;
    std::string proxyBypass;  // space separated
    bool ftpPassive = true;

    // Options > Sounds
    SoundEvent soundComplete;
    SoundEvent soundFailed;
    SoundEvent soundQueueStarted;
    SoundEvent soundQueueStopped;

    // Speed limiter (checklist Q-09)
    bool speedLimiterEnabled = false;
    int speedLimiterKilobytesPerSecond = 500;
    bool speedLimiterOnStartup = false;

    // View and shell
    bool hideCategories = false;
    bool largeToolbarButtons = true;
    bool darkModeSupport = true;
    TrayIconStyle trayIconStyle = TrayIconStyle::Color;
    DoubleClickAction doubleClickAction = DoubleClickAction::Open;
    std::string listFont;         // empty = system default; "face;pointSize"
    std::string columnLayout;     // owned by the list controller
    std::string toolbarLayout;    // owned by the toolbar controller
    std::string windowPlacement;  // owned by the main window
    std::string listSortKey;
    bool showDropTarget = false;
    std::string dropTargetPosition;  // owned by the drop target window

    // Recently used values offered again in the add dialogs (checklist A-01, A-02), newest first,
    // one per line.
    std::string addressHistory;
    std::string recentFolders;

    // Confirmations
    bool confirmDeleteCompleted = true;
    bool confirmDeleteIncomplete = true;
};

using SettingsMap = std::map<std::string, std::string, std::less<>>;

struct SettingsLoadResult {
    Settings settings;
    std::vector<std::string> warnings;  // one entry per invalid stored value that was replaced
};

// Builds settings from stored key/value pairs. Missing keys take defaults; invalid values take
// defaults and add a warning. Unknown keys are ignored.
[[nodiscard]] SettingsLoadResult SettingsFromMap(const SettingsMap& values);

// Serializes every setting. The result always contains every known key.
[[nodiscard]] SettingsMap SettingsToMap(const Settings& settings);

}  // namespace ldm::model
