#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace ldm::base {

// Standard base64 (RFC 4648, with padding). Nothing for anything else, including whitespace.
[[nodiscard]] std::optional<std::string> DecodeBase64(std::string_view text);

}  // namespace ldm::base
