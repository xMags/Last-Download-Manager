#include "browser/Protocol.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace ldm::browser {

namespace {

constexpr size_t kMaxIdChars = 64;
constexpr size_t kMaxTypeChars = 32;
constexpr size_t kMaxNameChars = 64;
constexpr size_t kMaxVersionChars = 32;
constexpr size_t kChromiumIdChars = 32;
constexpr size_t kMaxFirefoxIdChars = 80;
constexpr std::wstring_view kChromiumOriginPrefix = L"chrome-extension://";

bool IsPrintableAscii(std::string_view text)
{
    return std::all_of(text.begin(), text.end(), [](char c) { return c >= 0x20 && c < 0x7F; });
}

bool IsValidId(std::string_view id)
{
    return !id.empty() && id.size() <= kMaxIdChars && IsPrintableAscii(id);
}

bool IsValidType(std::string_view type)
{
    return !type.empty() && type.size() <= kMaxTypeChars &&
           std::all_of(type.begin(), type.end(), [](char c) { return (c >= 'a' && c <= 'z') || c == '-'; });
}

// Chromium extension ids are 32 letters from 'a' to 'p' (a hash written in base 16 with letters).
template <typename Char>
bool IsChromiumExtensionId(std::basic_string_view<Char> id)
{
    return id.size() == kChromiumIdChars &&
           std::all_of(id.begin(), id.end(), [](Char c) { return c >= Char('a') && c <= Char('p'); });
}

// Firefox add-on ids are an email-like name or a braced GUID.
template <typename Char>
bool IsFirefoxExtensionId(std::basic_string_view<Char> id)
{
    return !id.empty() && id.size() <= kMaxFirefoxIdChars && std::all_of(id.begin(), id.end(), [](Char c) {
        return (c >= Char('a') && c <= Char('z')) || (c >= Char('A') && c <= Char('Z')) || (c >= Char('0') && c <= Char('9')) ||
               c == Char('@') || c == Char('.') || c == Char('_') || c == Char('-') || c == Char('{') || c == Char('}');
    });
}

std::string Narrow(std::wstring_view ascii)
{
    std::string text;
    text.reserve(ascii.size());
    for (const wchar_t c : ascii) {
        text.push_back(static_cast<char>(c));  // callers checked that every character is ASCII
    }
    return text;
}

// A required string field of at most `maxChars` printable characters.
std::optional<std::string> Text(const nlohmann::json& object, const char* key, size_t maxChars)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return std::nullopt;
    }
    const auto& text = it->get_ref<const std::string&>();
    if (text.size() > maxChars || !IsPrintableAscii(text)) {
        return std::nullopt;
    }
    return text;
}

std::optional<bool> Boolean(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_boolean()) {
        return std::nullopt;
    }
    return it->get<bool>();
}

std::optional<HostInfo> ParseHostInfo(const nlohmann::json& object)
{
    const auto family = Text(object, "family", 16);
    const auto extensionId = Text(object, "extensionId", kMaxFirefoxIdChars);
    if (!family || !extensionId) {
        return std::nullopt;
    }
    if (*family == "chromium" && IsChromiumExtensionId(std::string_view(*extensionId))) {
        return HostInfo{BrowserFamily::Chromium, *extensionId};
    }
    if (*family == "firefox" && IsFirefoxExtensionId(std::string_view(*extensionId))) {
        return HostInfo{BrowserFamily::Firefox, *extensionId};
    }
    return std::nullopt;
}

std::optional<Hello> ParseHello(const nlohmann::json& object)
{
    auto browser = Text(object, "browser", kMaxNameChars);
    auto browserVersion = Text(object, "browserVersion", kMaxVersionChars);
    auto extensionVersion = Text(object, "extensionVersion", kMaxVersionChars);
    const auto incognitoAllowed = Boolean(object, "incognitoAllowed");
    if (!browser || browser->empty() || !browserVersion || !extensionVersion || extensionVersion->empty() ||
        !incognitoAllowed) {
        return std::nullopt;
    }
    return Hello{std::move(*browser), std::move(*browserVersion), std::move(*extensionVersion), *incognitoAllowed};
}

nlohmann::json Envelope(std::string_view id, std::string_view type)
{
    return {{"v", kProtocolVersion}, {"id", std::string(id)}, {"type", std::string(type)}};
}

}  // namespace

bool IsValidEnvelope(std::string_view json)
{
    if (json.empty() || json.size() > kMaxInboundBytes) {
        return false;
    }
    const nlohmann::json message = nlohmann::json::parse(json, nullptr, false);
    if (message.is_discarded() || !message.is_object()) {
        return false;
    }
    const auto version = message.find("v");
    const auto id = message.find("id");
    const auto type = message.find("type");
    return version != message.end() && version->is_number_integer() && version->get<std::int64_t>() == kProtocolVersion &&
           id != message.end() && id->is_string() && IsValidId(id->get_ref<const std::string&>()) &&
           type != message.end() && type->is_string() && IsValidType(type->get_ref<const std::string&>());
}

std::optional<ParsedMessage> ParseMessage(std::string_view json)
{
    if (!IsValidEnvelope(json)) {
        return std::nullopt;
    }
    const nlohmann::json message = nlohmann::json::parse(json);  // valid, checked above
    std::string id = message["id"].get<std::string>();
    const std::string& type = message["type"].get_ref<const std::string&>();
    if (type == "host") {
        if (auto info = ParseHostInfo(message)) {
            return ParsedMessage{std::move(id), std::move(*info)};
        }
    } else if (type == "hello") {
        if (auto hello = ParseHello(message)) {
            return ParsedMessage{std::move(id), std::move(*hello)};
        }
    }
    return std::nullopt;
}

std::optional<HostInfo> HostInfoFromArguments(const std::vector<std::wstring>& arguments)
{
    for (const std::wstring& argument : arguments) {
        const std::wstring_view text(argument);
        if (!text.starts_with(kChromiumOriginPrefix)) {
            continue;
        }
        std::wstring_view id = text.substr(kChromiumOriginPrefix.size());
        if (id.ends_with(L'/')) {
            id.remove_suffix(1);
        }
        if (!IsChromiumExtensionId(id)) {
            return std::nullopt;
        }
        return HostInfo{BrowserFamily::Chromium, Narrow(id)};
    }
    // Firefox: <path to the host manifest> <add-on id>
    if (arguments.size() >= 2 && IsFirefoxExtensionId(std::wstring_view(arguments[1]))) {
        return HostInfo{BrowserFamily::Firefox, Narrow(arguments[1])};
    }
    return std::nullopt;
}

std::string EncodeHostInfo(const HostInfo& info)
{
    nlohmann::json message = Envelope("host", "host");
    message["family"] = info.family == BrowserFamily::Firefox ? "firefox" : "chromium";
    message["extensionId"] = info.extensionId;
    return message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string EncodeConfig(const Config& config, std::string_view replyTo)
{
    nlohmann::json message = Envelope(replyTo, "config");
    message["appVersion"] = config.appVersion;
    message["capturedExtensions"] = config.capturedExtensions;
    return message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace ldm::browser
