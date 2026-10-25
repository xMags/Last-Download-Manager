#include "core/FileNames.h"

#include "base/Utf.h"

#include <algorithm>
#include <array>

namespace ldm::core {

namespace {

constexpr std::string_view kFallbackName = "download";
// Leaves room for " (999)" and the ".ldmpart" suffix inside the 255 UTF-16 unit component limit.
constexpr size_t kMaxNameUnits = 200;

constexpr std::array<std::wstring_view, 24> kDeviceNames{
    L"CON",  L"PRN",  L"AUX",  L"NUL",  L"CONIN$", L"CONOUT$", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6",
    L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6",   L"LPT7",    L"LPT8", L"LPT9",
};

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

bool IsDeviceName(std::wstring_view name)
{
    // "CON.txt" is as reserved as "CON".
    std::wstring stem(name.substr(0, name.find(L'.')));
    while (!stem.empty() && stem.back() == L' ') {
        stem.pop_back();
    }
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](wchar_t c) { return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - L'a' + L'A') : c; });
    return std::find(kDeviceNames.begin(), kDeviceNames.end(), stem) != kDeviceNames.end();
}

}  // namespace

std::string FileNameFromUrlPath(std::string_view path)
{
    path = path.substr(0, path.find_first_of("?#"));
    const size_t slash = path.rfind('/');
    const std::string_view segment = slash == std::string_view::npos ? path : path.substr(slash + 1);
    std::string decoded;
    decoded.reserve(segment.size());
    for (size_t i = 0; i < segment.size(); ++i) {
        if (segment[i] == '%' && i + 2 < segment.size()) {
            const int high = HexValue(segment[i + 1]);
            const int low = HexValue(segment[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        decoded.push_back(segment[i]);
    }
    return decoded;
}

std::string SanitizeFileName(std::string_view name)
{
    // Work in UTF-16, the unit Windows limits are expressed in. The round trip also replaces
    // invalid UTF-8 with U+FFFD.
    std::wstring wide = base::ToUtf16(name);
    std::wstring clean;
    clean.reserve(wide.size());
    for (const wchar_t c : wide) {
        if (c < 0x20 || c == 0x7F) {
            continue;
        }
        if (std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) {
            clean.push_back(L'_');
            continue;
        }
        clean.push_back(c);
    }
    const auto trimEnd = [&clean] {
        // Windows drops trailing dots and spaces, but not through the \\?\ paths LDM opens files
        // with, so a name ending in one would be a file Explorer cannot open or delete.
        while (!clean.empty() && (clean.back() == L' ' || clean.back() == L'.')) {
            clean.pop_back();
        }
    };
    trimEnd();
    const size_t first = clean.find_first_not_of(L' ');
    clean.erase(0, first == std::wstring::npos ? clean.size() : first);
    if (clean.empty()) {
        return std::string(kFallbackName);
    }
    if (IsDeviceName(clean)) {
        clean.insert(clean.begin(), L'_');
    }
    if (clean.size() > kMaxNameUnits) {
        const size_t dot = clean.rfind(L'.');
        const size_t extensionLength = dot == std::wstring::npos ? 0 : clean.size() - dot;
        std::wstring extension = extensionLength > 0 && extensionLength <= 16 ? clean.substr(dot) : std::wstring();
        size_t keep = kMaxNameUnits - extension.size();
        // Never cut a surrogate pair in half.
        if (keep > 0 && clean[keep - 1] >= 0xD800 && clean[keep - 1] <= 0xDBFF) {
            --keep;
        }
        clean = clean.substr(0, keep) + extension;
        trimEnd();  // the cut can end the name in a dot or space
        if (clean.empty()) {
            return std::string(kFallbackName);
        }
    }
    return base::ToUtf8(clean);
}

std::string FileExtension(std::string_view name)
{
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot + 1 == name.size() || dot == 0) {
        return {};
    }
    std::string extension(name.substr(dot + 1));
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; });
    return extension;
}

std::string NumberedFileName(std::string_view name, int number)
{
    const size_t dot = name.rfind('.');
    const std::string suffix = " (" + std::to_string(number) + ")";
    if (dot == std::string_view::npos || dot == 0) {
        return std::string(name) + suffix;
    }
    return std::string(name.substr(0, dot)) + suffix + std::string(name.substr(dot));
}

NumberedName SplitNumberedFileName(std::string_view name)
{
    const size_t dot = name.rfind('.');
    const bool hasExtension = dot != std::string_view::npos && dot != 0;
    const std::string_view stem = hasExtension ? name.substr(0, dot) : name;
    const std::string_view extension = hasExtension ? name.substr(dot) : std::string_view{};
    // Looks for " (digits)" at the end of the stem.
    if (stem.size() < 4 || stem.back() != ')') {
        return NumberedName{std::string(name), 1};
    }
    const size_t open = stem.rfind(" (");
    if (open == std::string_view::npos || open == 0) {
        return NumberedName{std::string(name), 1};
    }
    const std::string_view digits = stem.substr(open + 2, stem.size() - open - 3);
    if (digits.empty() || digits.size() > 3 || digits.front() == '0' ||
        !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return NumberedName{std::string(name), 1};
    }
    const int number = std::stoi(std::string(digits));
    if (number < 2) {
        return NumberedName{std::string(name), 1};
    }
    return NumberedName{std::string(stem.substr(0, open)) + std::string(extension), number};
}

}  // namespace ldm::core
