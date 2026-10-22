#pragma once

#include "core/Ports.h"

namespace ldm::storage {

// Passwords at rest (design section 5): CryptProtectData for the current Windows user, with a
// fixed application entropy and no UI. Only that user on that machine can decrypt them.
class DpapiSecretProtector final : public core::SecretProtector {
public:
    base::Result<std::vector<std::uint8_t>> Protect(std::string_view secret) override;
    base::Result<std::string> Unprotect(std::span<const std::uint8_t> protectedSecret) override;
};

}  // namespace ldm::storage
