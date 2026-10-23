#include "browser/Protocol.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <iterator>

namespace ldm::browser {

namespace {

constexpr size_t kMaxIdChars = 64;
constexpr size_t kMaxTypeChars = 32;
constexpr size_t kMaxNameChars = 64;
constexpr size_t kMaxVersionChars = 32;
constexpr size_t kChromiumIdChars = 32;
constexpr size_t kMaxFirefoxIdChars = 80;
constexpr std::wstring_view kChromiumOriginPrefix = L"chrome-extension://";
constexpr size_t kMaxUrl = 16 * 1024;
constexpr size_t kMaxFileName = 1024;
constexpr size_t kMaxUserAgent = 1024;
constexpr size_t kMaxMimeType = 256;
constexpr size_t kMaxCookies = 300;

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
    const auto startLdm = Boolean(object, "startLdm");
    if (!browser || browser->empty() || !browserVersion || !extensionVersion || extensionVersion->empty() ||
        !incognitoAllowed || !startLdm) {
        return std::nullopt;
    }
    return Hello{std::move(*browser), std::move(*browserVersion), std::move(*extensionVersion), *incognitoAllowed,
                 *startLdm};
}

bool HasNoControlCharacters(std::string_view text)
{
    return std::none_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; });
}

// An address field: a non-empty string without spaces or control characters, of a scheme LDM
// downloads. The full parse happens where the address is used.
bool IsDownloadAddress(const std::string& url)
{
    constexpr std::string_view kSchemes[] = {"http://", "https://", "ftp://", "ftps://"};
    if (url.empty() || url.size() > kMaxUrl || !HasNoControlCharacters(url) || url.find(' ') != std::string::npos) {
        return false;
    }
    return std::any_of(std::begin(kSchemes), std::end(kSchemes), [&url](std::string_view scheme) {
        return url.size() > scheme.size() && std::equal(scheme.begin(), scheme.end(), url.begin(), [](char a, char b) {
                   return a == static_cast<char>(std::tolower(static_cast<unsigned char>(b)));
               });
    });
}

// An optional string member: absent or null is nothing, anything else must pass `valid`.
template <typename Valid>
bool OptionalField(const nlohmann::json& object, const char* key, Valid valid, std::optional<std::string>& out)
{
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return true;
    }
    if (!it->is_string() || !valid(it->get_ref<const std::string&>())) {
        return false;
    }
    out = it->get<std::string>();
    return true;
}

std::optional<model::Cookie> ParseCookie(const nlohmann::json& object)
{
    if (!object.is_object()) {
        return std::nullopt;
    }
    const auto text = [&object](const char* key) -> const std::string* {
        const auto it = object.find(key);
        return it != object.end() && it->is_string() ? &it->get_ref<const std::string&>() : nullptr;
    };
    const auto flag = [&object](const char* key) -> std::optional<bool> {
        const auto it = object.find(key);
        return it != object.end() && it->is_boolean() ? std::optional<bool>(it->get<bool>()) : std::nullopt;
    };
    const auto* domain = text("domain");
    const auto* path = text("path");
    const auto* name = text("name");
    const auto* value = text("value");
    const auto hostOnly = flag("hostOnly");
    const auto secure = flag("secure");
    const auto expires = object.find("expires");
    if (domain == nullptr || path == nullptr || name == nullptr || value == nullptr || !hostOnly || !secure ||
        expires == object.end() || !expires->is_number_integer()) {
        return std::nullopt;
    }
    model::Cookie cookie{*domain, *hostOnly, *path, *secure, expires->get<std::int64_t>(), *name, *value};
    if (!model::IsValidCookie(cookie)) {
        return std::nullopt;
    }
    return cookie;
}

// The required "cookies" array of at most `max` valid cookies.
bool ParseCookies(const nlohmann::json& object, size_t max, std::vector<model::Cookie>& out)
{
    const auto cookies = object.find("cookies");
    if (cookies == object.end() || !cookies->is_array() || cookies->size() > max) {
        return false;
    }
    out.reserve(cookies->size());
    for (const auto& entry : *cookies) {
        auto cookie = ParseCookie(entry);
        if (!cookie) {
            return false;
        }
        out.push_back(std::move(*cookie));
    }
    return true;
}

// The "post" member of a capture: absent or null for none; otherwise the content type (one header
// value, so no line breaks) and the base64 body.
bool ParsePost(const nlohmann::json& object, std::optional<PostBody>& out)
{
    const auto post = object.find("post");
    if (post == object.end() || post->is_null()) {
        return true;
    }
    if (!post->is_object()) {
        return false;
    }
    const auto type = Text(*post, "contentType", kMaxMimeType);
    const auto body = post->find("body");
    if (!type || type->empty() || body == post->end() || !body->is_string() ||
        body->get_ref<const std::string&>().size() > (kMaxPostBytes / 3 + 1) * 4) {
        return false;
    }
    auto bytes = DecodeBase64(body->get_ref<const std::string&>());
    if (!bytes || bytes->size() > kMaxPostBytes) {
        return false;
    }
    out = PostBody{*type, std::move(*bytes)};
    return true;
}

std::optional<Capture> ParseCapture(const nlohmann::json& object)
{
    Capture capture;
    const auto url = object.find("url");
    if (url == object.end() || !url->is_string() || !IsDownloadAddress(url->get_ref<const std::string&>())) {
        return std::nullopt;
    }
    capture.url = url->get<std::string>();
    const auto plain = [](size_t max) {
        return [max](const std::string& text) { return !text.empty() && text.size() <= max && HasNoControlCharacters(text); };
    };
    if (!OptionalField(object, "finalUrl", IsDownloadAddress, capture.finalUrl) ||
        !OptionalField(object, "referrer", IsDownloadAddress, capture.referrer) ||
        !OptionalField(object, "fileName", plain(kMaxFileName), capture.fileName) ||
        !OptionalField(object, "mimeType", plain(kMaxMimeType), capture.mimeType)) {
        return std::nullopt;
    }
    const auto size = object.find("fileSize");
    if (size != object.end() && !size->is_null()) {
        if (!size->is_number_integer() || size->get<std::int64_t>() < 0) {
            return std::nullopt;
        }
        capture.fileSize = size->get<std::int64_t>();
    }
    auto userAgent = Text(object, "userAgent", kMaxUserAgent);
    if (!userAgent || userAgent->empty()) {
        return std::nullopt;
    }
    capture.userAgent = std::move(*userAgent);
    if (!ParsePost(object, capture.post) || !ParseCookies(object, kMaxCookies, capture.cookies)) {
        return std::nullopt;
    }
    return capture;
}

std::optional<LinkKind> ParseLinkKind(const std::string& kind)
{
    if (kind == "link") {
        return LinkKind::Link;
    }
    if (kind == "image") {
        return LinkKind::Image;
    }
    if (kind == "frame") {
        return LinkKind::Frame;
    }
    if (kind == "text") {
        return LinkKind::Text;
    }
    return std::nullopt;
}

std::optional<PageLink> ParsePageLink(const nlohmann::json& object)
{
    if (!object.is_object()) {
        return std::nullopt;
    }
    const auto url = object.find("url");
    const auto kind = Text(object, "kind", 16);
    if (url == object.end() || !url->is_string() || !IsDownloadAddress(url->get_ref<const std::string&>()) || !kind) {
        return std::nullopt;
    }
    const auto parsedKind = ParseLinkKind(*kind);
    if (!parsedKind) {
        return std::nullopt;
    }
    PageLink link;
    link.url = url->get<std::string>();
    link.kind = *parsedKind;
    // The parser has checked that strings are valid UTF-8; link texts are free text otherwise.
    std::optional<std::string> text;
    const auto plain = [](size_t max) {
        return [max](const std::string& value) { return value.size() <= max && HasNoControlCharacters(value); };
    };
    if (!OptionalField(object, "text", plain(kMaxLinkText), text) ||
        !OptionalField(object, "fileName", plain(kMaxFileName), link.fileName)) {
        return std::nullopt;
    }
    link.text = text.value_or(std::string());
    if (link.fileName && link.fileName->empty()) {
        link.fileName.reset();
    }
    return link;
}

std::optional<Links> ParseLinks(const nlohmann::json& object)
{
    Links links;
    const auto selection = Boolean(object, "selection");
    auto userAgent = Text(object, "userAgent", kMaxUserAgent);
    if (!selection || !userAgent || userAgent->empty() ||
        !OptionalField(object, "referrer", IsDownloadAddress, links.referrer)) {
        return std::nullopt;
    }
    links.selection = *selection;
    links.userAgent = std::move(*userAgent);
    const auto list = object.find("links");
    if (list == object.end() || !list->is_array() || list->empty() || list->size() > kMaxPageLinks) {
        return std::nullopt;
    }
    links.links.reserve(list->size());
    for (const auto& entry : *list) {
        auto link = ParsePageLink(entry);
        if (!link) {
            return std::nullopt;
        }
        links.links.push_back(std::move(*link));
    }
    if (!ParseCookies(object, kMaxPageLinkCookies, links.cookies)) {
        return std::nullopt;
    }
    return links;
}

std::optional<LinkDetails> ParseLinkDetails(const nlohmann::json& object)
{
    if (!object.is_object()) {
        return std::nullopt;
    }
    const auto index = object.find("index");
    if (index == object.end() || !index->is_number_unsigned() || index->get<std::uint64_t>() >= kMaxPageLinks) {
        return std::nullopt;
    }
    LinkDetails details;
    details.index = static_cast<size_t>(index->get<std::uint64_t>());
    const auto failed = object.find("failed");
    if (failed != object.end() && !failed->is_null()) {
        if (!failed->is_boolean()) {
            return std::nullopt;
        }
        details.failed = failed->get<bool>();
    }
    const auto plain = [](size_t max) {
        return [max](const std::string& value) { return !value.empty() && value.size() <= max && HasNoControlCharacters(value); };
    };
    if (!OptionalField(object, "mimeType", plain(kMaxMimeType), details.mimeType) ||
        !OptionalField(object, "fileName", plain(kMaxFileName), details.fileName)) {
        return std::nullopt;
    }
    const auto size = object.find("fileSize");
    if (size != object.end() && !size->is_null()) {
        if (!size->is_number_integer() || size->get<std::int64_t>() < 0) {
            return std::nullopt;
        }
        details.fileSize = size->get<std::int64_t>();
    }
    return details;
}

std::optional<LinkInfo> ParseLinkInfo(const nlohmann::json& object)
{
    const auto items = object.find("items");
    if (items == object.end() || !items->is_array() || items->size() > kMaxPageLinks) {
        return std::nullopt;
    }
    LinkInfo info;
    for (const auto& entry : *items) {
        auto details = ParseLinkDetails(entry);
        if (!details) {
            return std::nullopt;
        }
        info.items.push_back(std::move(*details));
    }
    return info;
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

bool HelloAsksToStartLdm(std::string_view json)
{
    const auto parsed = ParseMessage(json);
    const auto* hello = parsed ? std::get_if<Hello>(&parsed->message) : nullptr;
    return hello != nullptr && hello->startLdm;
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
    } else if (type == "capture") {
        if (auto capture = ParseCapture(message)) {
            return ParsedMessage{std::move(id), std::move(*capture)};
        }
    } else if (type == "links") {
        if (auto links = ParseLinks(message)) {
            return ParsedMessage{std::move(id), std::move(*links)};
        }
    } else if (type == "link-info") {
        if (auto info = ParseLinkInfo(message)) {
            return ParsedMessage{std::move(id), std::move(*info)};
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
    message["siteExceptions"] = config.siteExceptions;
    message["addressExceptions"] = config.addressExceptions;
    const model::CaptureKeys& keys = config.keys;
    message["keys"] = {
        {"prevent", {{"enabled", keys.preventEnabled}, {"alt", keys.preventAlt}, {"shift", keys.preventShift},
                     {"ctrl", keys.preventCtrl}}},
        {"force", {{"enabled", keys.forceEnabled}, {"alt", keys.forceAlt}, {"shift", keys.forceShift},
                   {"ctrl", keys.forceCtrl}, {"insert", keys.forceInsert}, {"delete", keys.forceDelete}}},
        {"forceNeedsLinkClick", keys.forceNeedsLinkClick},
        {"skipWebContent", keys.skipWebContent},
    };
    message["menu"] = {{"downloadLink", config.menu.downloadLink}, {"downloadAll", config.menu.downloadAll}};
    return message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string EncodeCaptureResult(std::string_view replyTo, bool accepted)
{
    nlohmann::json message = Envelope(replyTo, "capture-result");
    message["accepted"] = accepted;
    return message.dump();
}

std::string EncodeLinksResult(std::string_view replyTo, bool accepted)
{
    nlohmann::json message = Envelope(replyTo, "links-result");
    message["accepted"] = accepted;
    return message.dump();
}

std::string EncodeLinksDone(std::string_view replyTo)
{
    return Envelope(replyTo, "links-done").dump();
}

std::optional<std::string> DecodeBase64(std::string_view text)
{
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '+') {
            return 62;
        }
        if (c == '/') {
            return 63;
        }
        return -1;
    };
    std::string bytes;
    bytes.reserve(text.size() / 4 * 3);
    for (size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        const size_t padding = last ? (text[i + 3] == '=' ? (text[i + 2] == '=' ? 2 : 1) : 0) : 0;
        std::uint32_t group = 0;
        for (size_t j = 0; j < 4; ++j) {
            const char c = text[i + j];
            const int v = j >= 4 - padding ? 0 : value(c);
            if (v < 0 || (j < 4 - padding && c == '=')) {
                return std::nullopt;
            }
            group = (group << 6) | static_cast<std::uint32_t>(v);
        }
        bytes.push_back(static_cast<char>((group >> 16) & 0xFF));
        if (padding < 2) {
            bytes.push_back(static_cast<char>((group >> 8) & 0xFF));
        }
        if (padding < 1) {
            bytes.push_back(static_cast<char>(group & 0xFF));
        }
    }
    return bytes;
}

}  // namespace ldm::browser
