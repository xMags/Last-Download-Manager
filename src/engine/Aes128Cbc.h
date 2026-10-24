#pragma once

#include "model/Media.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ldm::engine {

// Decrypts one HLS AES-128 segment as its bytes arrive (RFC 8216 section 4.3.2.4): AES-128 in
// CBC mode with PKCS#7 padding, with Windows CNG. The plaintext of the last block is held back
// until Finish, which checks and removes the padding. Not thread-safe; one per segment.
class Aes128CbcDecryptor {
public:
    // Nothing when Windows refuses the key, which does not happen for 16 bytes in practice.
    [[nodiscard]] static std::optional<Aes128CbcDecryptor> Create(const model::AesKey& key, const model::AesKey& iv);

    Aes128CbcDecryptor(Aes128CbcDecryptor&&) noexcept;
    Aes128CbcDecryptor& operator=(Aes128CbcDecryptor&&) noexcept;
    Aes128CbcDecryptor(const Aes128CbcDecryptor&) = delete;
    Aes128CbcDecryptor& operator=(const Aes128CbcDecryptor&) = delete;
    ~Aes128CbcDecryptor();

    // Appends to `plain` what `cipher` completes. False when decryption failed; the segment is
    // then unusable.
    [[nodiscard]] bool Update(std::span<const std::byte> cipher, std::vector<std::byte>& plain);
    // The rest of the plaintext, without its padding. False when the segment was not a whole
    // number of blocks or its padding is wrong: a wrong key, or a cut segment.
    [[nodiscard]] bool Finish(std::vector<std::byte>& plain);

private:
    struct Key;
    Aes128CbcDecryptor(std::unique_ptr<Key> key, const model::AesKey& iv);

    bool Decrypt(std::span<std::byte> blocks);

    std::unique_ptr<Key> key_;
    model::AesKey iv_;           // CNG carries the chain forward in it
    std::vector<std::byte> carry_;  // ciphertext not yet a whole block
    std::vector<std::byte> held_;   // plaintext of the last whole block, which may be padding
};

}  // namespace ldm::engine
