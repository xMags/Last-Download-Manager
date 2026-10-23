#include "base/Base64.h"

#include <cstdint>

namespace ldm::base {

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

}  // namespace ldm::base
