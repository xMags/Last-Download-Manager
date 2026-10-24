#include "model/Settings.h"

#include <array>
#include <charconv>
#include <string_view>
#include <utility>

namespace ldm::model {

namespace {

constexpr size_t kMaxStringLength = 32 * 1024;

template <typename Enum, size_t N>
using EnumNames = std::array<std::pair<Enum, std::string_view>, N>;

constexpr EnumNames<DuplicatePolicy, 4> kDuplicatePolicyNames{{
    {DuplicatePolicy::Ask, "ask"},
    {DuplicatePolicy::AddNumbered, "numbered"},
    {DuplicatePolicy::Overwrite, "overwrite"},
    {DuplicatePolicy::ResumeOrShowComplete, "resume"},
}};

constexpr EnumNames<ProgressStartView, 3> kStartViewNames{{
    {ProgressStartView::Normal, "normal"},
    {ProgressStartView::Minimized, "minimized"},
    {ProgressStartView::Hidden, "hidden"},
}};

constexpr EnumNames<SpeedProfile, 3> kSpeedProfileNames{{
    {SpeedProfile::Low, "low"},
    {SpeedProfile::Medium, "medium"},
    {SpeedProfile::High, "high"},
}};

constexpr EnumNames<ProxyMode, 4> kProxyModeNames{{
    {ProxyMode::None, "none"},
    {ProxyMode::System, "system"},
    {ProxyMode::AutoConfigScript, "pac"},
    {ProxyMode::Manual, "manual"},
}};

constexpr EnumNames<TrayIconStyle, 3> kTrayIconNames{{
    {TrayIconStyle::Color, "color"},
    {TrayIconStyle::Classic, "classic"},
    {TrayIconStyle::Hidden, "hidden"},
}};

constexpr EnumNames<DoubleClickAction, 2> kDoubleClickNames{{
    {DoubleClickAction::Open, "open"},
    {DoubleClickAction::Properties, "properties"},
}};

constexpr std::string_view kHexDigits = "0123456789abcdef";

std::string EncodeHex(const std::vector<std::uint8_t>& bytes)
{
    std::string text;
    text.reserve(bytes.size() * 2);
    for (std::uint8_t byte : bytes) {
        text.push_back(kHexDigits[byte >> 4]);
        text.push_back(kHexDigits[byte & 0x0F]);
    }
    return text;
}

int HexValue(char c) noexcept
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool DecodeHex(std::string_view text, std::vector<std::uint8_t>& out)
{
    if (text.size() % 2 != 0) {
        return false;
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        const int high = HexValue(text[i]);
        const int low = HexValue(text[i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    out = std::move(bytes);
    return true;
}

// Reads fields from stored values. A missing key leaves the default in place; an invalid value
// also leaves the default in place and records a warning naming the key.
class Reader {
public:
    Reader(const SettingsMap& values, std::vector<std::string>& warnings)
        : values_(values), warnings_(warnings)
    {
    }

    void Bool(std::string_view key, bool& field)
    {
        const std::string* text = Find(key);
        if (text == nullptr) {
            return;
        }
        if (*text == "1") {
            field = true;
        } else if (*text == "0") {
            field = false;
        } else {
            Warn(key);
        }
    }

    void Int(std::string_view key, int& field, int minimum, int maximum)
    {
        const std::string* text = Find(key);
        if (text == nullptr) {
            return;
        }
        int value = 0;
        const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), value);
        if (error != std::errc{} || end != text->data() + text->size() || value < minimum ||
            value > maximum) {
            Warn(key);
            return;
        }
        field = value;
    }

    void String(std::string_view key, std::string& field)
    {
        const std::string* text = Find(key);
        if (text == nullptr) {
            return;
        }
        if (text->size() > kMaxStringLength) {
            Warn(key);
            return;
        }
        field = *text;
    }

    void Bytes(std::string_view key, std::vector<std::uint8_t>& field)
    {
        const std::string* text = Find(key);
        if (text == nullptr) {
            return;
        }
        if (text->size() > kMaxStringLength || !DecodeHex(*text, field)) {
            Warn(key);
        }
    }

    template <typename Enum, size_t N>
    void Choice(std::string_view key, Enum& field, const EnumNames<Enum, N>& names)
    {
        const std::string* text = Find(key);
        if (text == nullptr) {
            return;
        }
        for (const auto& [value, name] : names) {
            if (name == *text) {
                field = value;
                return;
            }
        }
        Warn(key);
    }

private:
    const std::string* Find(std::string_view key) const
    {
        const auto it = values_.find(key);
        return it == values_.end() ? nullptr : &it->second;
    }

    void Warn(std::string_view key) { warnings_.push_back("invalid value for setting " + std::string(key)); }

    const SettingsMap& values_;
    std::vector<std::string>& warnings_;
};

class Writer {
public:
    explicit Writer(SettingsMap& values) : values_(values) {}

    void Bool(std::string_view key, const bool& field) { Put(key, field ? "1" : "0"); }
    void Int(std::string_view key, const int& field, int, int) { Put(key, std::to_string(field)); }
    void String(std::string_view key, const std::string& field) { Put(key, field); }
    void Bytes(std::string_view key, const std::vector<std::uint8_t>& field) { Put(key, EncodeHex(field)); }

    template <typename Enum, size_t N>
    void Choice(std::string_view key, const Enum& field, const EnumNames<Enum, N>& names)
    {
        for (const auto& [value, name] : names) {
            if (value == field) {
                Put(key, std::string(name));
                return;
            }
        }
    }

private:
    void Put(std::string_view key, std::string value) { values_.insert_or_assign(std::string(key), std::move(value)); }

    SettingsMap& values_;
};

template <typename Visitor, typename Server>
void VisitProxy(Visitor& visitor, std::string_view prefix, Server& server)
{
    const std::string base(prefix);
    visitor.Bool(base + ".enabled", server.enabled);
    visitor.String(base + ".host", server.host);
    visitor.Int(base + ".port", server.port, 0, 65535);
    visitor.String(base + ".user", server.user);
    visitor.Bytes(base + ".secret", server.secret);
}

template <typename Visitor>
void VisitSound(Visitor& visitor, std::string_view prefix, auto& sound)
{
    const std::string base(prefix);
    visitor.Bool(base + ".enabled", sound.enabled);
    visitor.String(base + ".file", sound.file);
}

// The single list of settings keys. Reading and writing both go through it, so a key can never
// be saved under one name and loaded under another. `Settings` is const for the Writer.
template <typename Visitor, typename SettingsType>
void VisitSettings(Visitor& v, SettingsType& s)
{
    v.Bool("general.launchOnStartup", s.launchOnStartup);
    v.Bool("general.monitorClipboard", s.monitorClipboard);

    v.String("fileTypes.captured", s.capturedExtensions);
    v.String("fileTypes.siteExceptions", s.captureSiteExceptions);
    v.String("fileTypes.addressExceptions", s.captureAddressExceptions);
    v.Bool("browser.keys.prevent.enabled", s.captureKeys.preventEnabled);
    v.Bool("browser.keys.prevent.alt", s.captureKeys.preventAlt);
    v.Bool("browser.keys.prevent.shift", s.captureKeys.preventShift);
    v.Bool("browser.keys.prevent.ctrl", s.captureKeys.preventCtrl);
    v.Bool("browser.keys.force.enabled", s.captureKeys.forceEnabled);
    v.Bool("browser.keys.force.alt", s.captureKeys.forceAlt);
    v.Bool("browser.keys.force.shift", s.captureKeys.forceShift);
    v.Bool("browser.keys.force.ctrl", s.captureKeys.forceCtrl);
    v.Bool("browser.keys.force.insert", s.captureKeys.forceInsert);
    v.Bool("browser.keys.force.delete", s.captureKeys.forceDelete);
    v.Bool("browser.keys.forceNeedsLinkClick", s.captureKeys.forceNeedsLinkClick);
    v.Bool("browser.keys.skipWebContent", s.captureKeys.skipWebContent);
    v.Bool("browser.offerExceptions", s.offerCaptureExceptions);
    v.Bool("browser.menu.chromium.downloadLink", s.chromiumMenu.downloadLink);
    v.Bool("browser.menu.chromium.downloadAll", s.chromiumMenu.downloadAll);
    v.Bool("browser.menu.firefox.downloadLink", s.firefoxMenu.downloadLink);
    v.Bool("browser.menu.firefox.downloadAll", s.firefoxMenu.downloadAll);
    v.Bool("media.panelEnabled", s.mediaPanelEnabled);
    v.String("media.types", s.mediaTypes);
    v.Int("media.minimumKilobytes", s.mediaMinimumKilobytes, 0, 10'000'000);
    v.String("media.siteExceptions", s.mediaSiteExceptions);
    v.Bool("media.drmNotice", s.mediaDrmNotice);
    v.Bool("allLinks.useLinkText", s.allLinksUseLinkText);
    v.Bool("allLinks.hideHtml", s.allLinksHideHtml);
    v.Bool("allLinks.hideImages", s.allLinksHideImages);
    v.Bool("allLinks.hideRepeated", s.allLinksHideRepeated);

    v.String("saveTo.temporaryDirectory", s.temporaryDirectory);
    v.Bool("saveTo.setFileDateFromServer", s.setFileDateFromServer);

    v.Bool("downloads.showStartDialog", s.showStartDialog);
    v.Bool("downloads.queueOnly", s.queueOnly);
    v.Bool("downloads.showCompleteDialog", s.showCompleteDialog);
    v.Bool("downloads.speculativeStart", s.speculativeStart);
    v.Bool("downloads.queuePanelOnLater", s.queuePanelOnLater);
    v.Bool("downloads.queuePanelOnBatch", s.queuePanelOnBatch);
    v.Bool("downloads.ignoreModificationTimeChanges", s.ignoreModificationTimeChanges);
    v.Choice("downloads.duplicatePolicy", s.duplicatePolicy, kDuplicatePolicyNames);
    v.String("downloads.userAgent", s.userAgent);
    v.String("downloads.virusScan.program", s.virusScan.program);
    v.String("downloads.virusScan.arguments", s.virusScan.arguments);

    v.Bool("progress.showSpeedTab", s.progressShowSpeedTab);
    v.Bool("progress.showCompletionTab", s.progressShowCompletionTab);
    v.Bool("progress.showHideTabButtons", s.progressShowHideTabButtons);
    v.Choice("progress.startView", s.progressStartView, kStartViewNames);

    v.Choice("connection.speedProfile", s.speedProfile, kSpeedProfileNames);
    v.Int("connection.defaultMaxConnections", s.defaultMaxConnections, 1, 32);
    v.Bool("connection.quota.enabled", s.quota.enabled);
    v.Int("connection.quota.megabytes", s.quota.megabytes, 1, 10'000'000);
    v.Int("connection.quota.hours", s.quota.hours, 1, 23);
    v.Bool("connection.quota.warn", s.quota.warnBeforeStopping);

    v.Choice("proxy.mode", s.proxyMode, kProxyModeNames);
    v.String("proxy.autoConfigScriptUrl", s.autoConfigScriptUrl);
    VisitProxy(v, "proxy.http", s.httpProxy);
    VisitProxy(v, "proxy.https", s.httpsProxy);
    VisitProxy(v, "proxy.ftp", s.ftpProxy);
    VisitProxy(v, "proxy.socks", s.socks);
    v.Int("proxy.socks.version", s.socks.version, 4, 5);
    v.Bool("proxy.socks.remoteDns", s.socks.remoteDns);
    v.String("proxy.bypass", s.proxyBypass);
    v.Bool("proxy.ftpPassive", s.ftpPassive);

    VisitSound(v, "sounds.complete", s.soundComplete);
    VisitSound(v, "sounds.failed", s.soundFailed);
    VisitSound(v, "sounds.queueStarted", s.soundQueueStarted);
    VisitSound(v, "sounds.queueStopped", s.soundQueueStopped);

    v.Bool("speedLimiter.enabled", s.speedLimiterEnabled);
    v.Int("speedLimiter.kilobytesPerSecond", s.speedLimiterKilobytesPerSecond, 1, 10'000'000);
    v.Bool("speedLimiter.onStartup", s.speedLimiterOnStartup);

    v.Bool("view.hideCategories", s.hideCategories);
    v.Bool("view.largeToolbarButtons", s.largeToolbarButtons);
    v.Bool("view.darkModeSupport", s.darkModeSupport);
    v.Choice("view.trayIcon", s.trayIconStyle, kTrayIconNames);
    v.Choice("view.doubleClick", s.doubleClickAction, kDoubleClickNames);
    v.String("view.listFont", s.listFont);
    v.String("view.columnLayout", s.columnLayout);
    v.String("view.toolbarLayout", s.toolbarLayout);
    v.String("view.windowPlacement", s.windowPlacement);
    v.String("view.listSortKey", s.listSortKey);
    v.Bool("view.showDropTarget", s.showDropTarget);
    v.String("view.dropTargetPosition", s.dropTargetPosition);

    v.String("history.addresses", s.addressHistory);
    v.String("history.folders", s.recentFolders);

    v.Bool("confirm.deleteCompleted", s.confirmDeleteCompleted);
    v.Bool("confirm.deleteIncomplete", s.confirmDeleteIncomplete);
}

}  // namespace

std::string DefaultUserAgent()
{
    return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
           "Chrome/141.0.0.0 Safari/537.36";
}

std::string DefaultCapturedExtensions()
{
    return "3GP 7Z AAC ACE AIF APK ARJ ASF AVI BIN BZ2 DMG DOC DOCX EXE FLAC FLV GZ GZIP IMG ISO LZH "
           "M4A M4V MKV MOV MP3 MP4 MPA MPE MPEG MPG MSI MSU OGG OGV OPUS PDF PPS PPT PPTX QT R0* R1* RA "
           "RAR RM RMVB SEA SIT SITX TAR TIF TIFF TORRENT WAV WEBM WMA WMV XLS XLSX XZ Z ZIP ZST";
}

std::string DefaultCaptureSiteExceptions()
{
    // Windows Update fetches its packages through the browser download machinery on some systems.
    return "*.update.microsoft.com download.windowsupdate.com";
}

std::string DefaultMediaTypes()
{
    return "3GP AAC ASF AVI DFXP F4V FLV M4A M4V MKV MOV MP3 MP4 MPEG MPG OGG OGV QT RM SRT TS TTML VTT WAV "
           "WEBM WMA WMV";
}

SettingsLoadResult SettingsFromMap(const SettingsMap& values)
{
    SettingsLoadResult result;
    Reader reader(values, result.warnings);
    VisitSettings(reader, result.settings);
    return result;
}

SettingsMap SettingsToMap(const Settings& settings)
{
    SettingsMap values;
    Writer writer(values);
    VisitSettings(writer, settings);
    return values;
}

}  // namespace ldm::model
