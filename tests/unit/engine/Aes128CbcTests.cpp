#include "engine/Aes128Cbc.h"

#include <windows.h>
#include <bcrypt.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace {

using ldm::engine::Aes128CbcDecryptor;
using ldm::model::AesKey;

std::vector<std::byte> FromHex(const std::string& hex)
{
    std::vector<std::byte> bytes;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        bytes.push_back(static_cast<std::byte>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

AesKey KeyFromHex(const std::string& hex)
{
    const auto bytes = FromHex(hex);
    AesKey key{};
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = std::to_integer<std::uint8_t>(bytes[i]);
    }
    return key;
}

// What an HLS packager does: CBC with PKCS#7 padding, here through CNG directly.
std::vector<std::byte> Encrypt(const AesKey& key, AesKey iv, const std::vector<std::byte>& plain)
{
    BCRYPT_KEY_HANDLE handle = nullptr;
    AesKey secret = key;
    EXPECT_TRUE(BCRYPT_SUCCESS(::BCryptGenerateSymmetricKey(BCRYPT_AES_CBC_ALG_HANDLE, &handle, nullptr, 0,
                                                            secret.data(), static_cast<ULONG>(secret.size()), 0)));
    std::vector<std::byte> cipher(plain.size() + 16);
    ULONG written = 0;
    // CNG reads nothing from a null input, not even to pad it; an empty segment still pads.
    std::byte none{};
    auto* input = reinterpret_cast<PUCHAR>(plain.empty() ? &none : const_cast<std::byte*>(plain.data()));
    EXPECT_TRUE(BCRYPT_SUCCESS(::BCryptEncrypt(handle, input, static_cast<ULONG>(plain.size()), nullptr, iv.data(),
                                               static_cast<ULONG>(iv.size()), reinterpret_cast<PUCHAR>(cipher.data()),
                                               static_cast<ULONG>(cipher.size()), &written, BCRYPT_BLOCK_PADDING)));
    ::BCryptDestroyKey(handle);
    cipher.resize(written);
    return cipher;
}

std::vector<std::byte> DecryptInChunks(const AesKey& key, const AesKey& iv, const std::vector<std::byte>& cipher,
                                       size_t chunk, bool& ok)
{
    auto decryptor = Aes128CbcDecryptor::Create(key, iv);
    EXPECT_TRUE(decryptor.has_value());
    std::vector<std::byte> plain;
    ok = true;
    for (size_t at = 0; at < cipher.size(); at += chunk) {
        const size_t size = std::min(chunk, cipher.size() - at);
        ok = ok && decryptor->Update(std::span<const std::byte>(cipher.data() + at, size), plain);
    }
    ok = ok && decryptor->Finish(plain);
    return plain;
}

// NIST SP 800-38A, F.2.2 CBC-AES128.Decrypt.
TEST(Aes128Cbc, MatchesTheNistVectorBlockByBlock)
{
    const AesKey key = KeyFromHex("2b7e151628aed2a6abf7158809cf4f3c");
    const AesKey iv = KeyFromHex("000102030405060708090a0b0c0d0e0f");
    const auto cipher = FromHex(
        "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
        "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7");
    const auto expected = FromHex(
        "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
        "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
    auto decryptor = Aes128CbcDecryptor::Create(key, iv);
    ASSERT_TRUE(decryptor);
    std::vector<std::byte> plain;
    for (size_t at = 0; at < cipher.size(); at += 5) {
        const size_t size = std::min<size_t>(5, cipher.size() - at);
        ASSERT_TRUE(decryptor->Update(std::span<const std::byte>(cipher.data() + at, size), plain));
    }
    // The last block is held back as possible padding.
    ASSERT_EQ(plain.size(), expected.size() - 16);
    EXPECT_TRUE(std::equal(plain.begin(), plain.end(), expected.begin()));
    // The vector has no padding, and its last byte (0x10) does not make one: refused.
    EXPECT_FALSE(decryptor->Finish(plain));
}

TEST(Aes128Cbc, RemovesPaddingForAnySplitOfTheSegment)
{
    const AesKey key = KeyFromHex("00112233445566778899aabbccddeeff");
    const AesKey iv = KeyFromHex("0f0e0d0c0b0a09080706050403020100");
    for (const size_t length : {0u, 1u, 15u, 16u, 17u, 188u * 7u}) {
        std::vector<std::byte> plain(length);
        for (size_t i = 0; i < length; ++i) {
            plain[i] = static_cast<std::byte>(i * 31 + 7);
        }
        const auto cipher = Encrypt(key, iv, plain);
        for (const size_t chunk : {1u, 7u, 16u, 33u, 4096u}) {
            bool ok = false;
            const auto decrypted = DecryptInChunks(key, iv, cipher, chunk, ok);
            EXPECT_TRUE(ok) << length << " bytes in chunks of " << chunk;
            EXPECT_EQ(decrypted, plain) << length << " bytes in chunks of " << chunk;
        }
    }
}

TEST(Aes128Cbc, RefusesACutSegmentAndAWrongKey)
{
    const AesKey key = KeyFromHex("00112233445566778899aabbccddeeff");
    const AesKey iv{};
    const std::vector<std::byte> plain(100, std::byte{0x42});
    const auto cipher = Encrypt(key, iv, plain);

    std::vector<std::byte> cut(cipher.begin(), cipher.end() - 5);
    bool ok = true;
    (void)DecryptInChunks(key, iv, cut, 16, ok);
    EXPECT_FALSE(ok);

    // A wrong key leaves random padding bytes: almost always refused, never the plaintext.
    bool wrongOk = true;
    const auto wrong = DecryptInChunks(KeyFromHex("ffeeddccbbaa99887766554433221100"), iv, cipher, 16, wrongOk);
    EXPECT_NE(wrong, plain);
}

}  // namespace
