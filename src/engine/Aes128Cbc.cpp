#include "engine/Aes128Cbc.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <utility>

namespace ldm::engine {

namespace {

constexpr size_t kBlock = 16;

}  // namespace

struct Aes128CbcDecryptor::Key {
    BCRYPT_KEY_HANDLE handle = nullptr;

    Key() = default;
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
    ~Key()
    {
        if (handle != nullptr) {
            ::BCryptDestroyKey(handle);
        }
    }
};

std::optional<Aes128CbcDecryptor> Aes128CbcDecryptor::Create(const model::AesKey& key, const model::AesKey& iv)
{
    auto handle = std::make_unique<Key>();
    // CNG's key import takes a non-const buffer it does not modify.
    model::AesKey secret = key;
    const NTSTATUS status = ::BCryptGenerateSymmetricKey(BCRYPT_AES_CBC_ALG_HANDLE, &handle->handle, nullptr, 0,
                                                         secret.data(), static_cast<ULONG>(secret.size()), 0);
    ::SecureZeroMemory(secret.data(), secret.size());
    if (!BCRYPT_SUCCESS(status)) {
        return std::nullopt;
    }
    return Aes128CbcDecryptor(std::move(handle), iv);
}

Aes128CbcDecryptor::Aes128CbcDecryptor(std::unique_ptr<Key> key, const model::AesKey& iv)
    : key_(std::move(key)), iv_(iv)
{
}

Aes128CbcDecryptor::Aes128CbcDecryptor(Aes128CbcDecryptor&&) noexcept = default;
Aes128CbcDecryptor& Aes128CbcDecryptor::operator=(Aes128CbcDecryptor&&) noexcept = default;
Aes128CbcDecryptor::~Aes128CbcDecryptor() = default;

bool Aes128CbcDecryptor::Decrypt(std::span<std::byte> blocks)
{
    ULONG written = 0;
    // In place, without padding: the padding is checked by Finish on the last block only. CNG
    // updates iv_ to the last ciphertext block, which chains the next call.
    auto* data = reinterpret_cast<PUCHAR>(blocks.data());  // CNG takes bytes as unsigned char
    const NTSTATUS status =
        ::BCryptDecrypt(key_->handle, data, static_cast<ULONG>(blocks.size()), nullptr, iv_.data(),
                        static_cast<ULONG>(iv_.size()), data, static_cast<ULONG>(blocks.size()), &written, 0);
    return BCRYPT_SUCCESS(status) && written == blocks.size();
}

bool Aes128CbcDecryptor::Update(std::span<const std::byte> cipher, std::vector<std::byte>& plain)
{
    carry_.insert(carry_.end(), cipher.begin(), cipher.end());
    const size_t whole = carry_.size() / kBlock * kBlock;
    if (whole == 0) {
        return true;
    }
    if (!Decrypt(std::span<std::byte>(carry_.data(), whole))) {
        return false;
    }
    // What was held is no longer the last block; the new last block is held instead.
    plain.insert(plain.end(), held_.begin(), held_.end());
    plain.insert(plain.end(), carry_.begin(), carry_.begin() + static_cast<std::ptrdiff_t>(whole - kBlock));
    held_.assign(carry_.begin() + static_cast<std::ptrdiff_t>(whole - kBlock),
                 carry_.begin() + static_cast<std::ptrdiff_t>(whole));
    carry_.erase(carry_.begin(), carry_.begin() + static_cast<std::ptrdiff_t>(whole));
    return true;
}

bool Aes128CbcDecryptor::Finish(std::vector<std::byte>& plain)
{
    if (!carry_.empty() || held_.size() != kBlock) {
        return false;
    }
    const auto pad = std::to_integer<size_t>(held_.back());
    if (pad == 0 || pad > kBlock) {
        return false;
    }
    const bool padded = std::all_of(held_.end() - static_cast<std::ptrdiff_t>(pad), held_.end(),
                                    [pad](std::byte b) { return std::to_integer<size_t>(b) == pad; });
    if (!padded) {
        return false;
    }
    plain.insert(plain.end(), held_.begin(), held_.end() - static_cast<std::ptrdiff_t>(pad));
    held_.clear();
    return true;
}

}  // namespace ldm::engine
