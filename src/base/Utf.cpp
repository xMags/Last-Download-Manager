#include "base/Utf.h"

#include <windows.h>

#include <limits>
#include <stdexcept>

namespace ldm::base {

namespace {

int CheckedLength(size_t length)
{
    if (length > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("string too long for Win32 conversion");
    }
    return static_cast<int>(length);
}

}  // namespace

std::wstring ToUtf16(std::string_view utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int inputLength = CheckedLength(utf8.size());
    const int outputLength = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), inputLength, nullptr, 0);
    if (outputLength <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(outputLength), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), inputLength, result.data(), outputLength);
    return result;
}

std::string ToUtf8(std::wstring_view utf16)
{
    if (utf16.empty()) {
        return {};
    }
    const int inputLength = CheckedLength(utf16.size());
    const int outputLength =
        ::WideCharToMultiByte(CP_UTF8, 0, utf16.data(), inputLength, nullptr, 0, nullptr, nullptr);
    if (outputLength <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(outputLength), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, utf16.data(), inputLength, result.data(), outputLength, nullptr,
                          nullptr);
    return result;
}

}  // namespace ldm::base
