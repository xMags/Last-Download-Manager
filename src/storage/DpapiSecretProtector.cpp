#include "storage/DpapiSecretProtector.h"

#include <windows.h>
#include <dpapi.h>

#include <limits>
#include <system_error>

namespace ldm::storage {

namespace {

// Ties protected blobs to LDM, so another program running as the same user cannot simply call
// CryptUnprotectData on them without knowing this value.
constexpr char kEntropy[] = "LastDownloadManager.Secrets.v1";

class LocalBlob {
public:
    LocalBlob() = default;
    LocalBlob(const LocalBlob&) = delete;
    LocalBlob& operator=(const LocalBlob&) = delete;
    ~LocalBlob()
    {
        if (blob.pbData != nullptr) {
            ::SecureZeroMemory(blob.pbData, blob.cbData);
            ::LocalFree(blob.pbData);
        }
    }
    DATA_BLOB blob{};
};

DATA_BLOB EntropyBlob()
{
    DATA_BLOB entropy{};
    entropy.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy));  // DPAPI reads it only
    entropy.cbData = static_cast<DWORD>(sizeof(kEntropy) - 1);
    return entropy;
}

base::Error LastError(std::string_view what)
{
    return base::MakeError(base::ErrorCode::Unknown,
                           std::string(what) + ": " + std::system_category().message(static_cast<int>(::GetLastError())));
}

}  // namespace

base::Result<std::vector<std::uint8_t>> DpapiSecretProtector::Protect(std::string_view secret)
{
    if (secret.size() > std::numeric_limits<DWORD>::max()) {
        return base::MakeError(base::ErrorCode::InvalidArgument, "secret too long");
    }
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(secret.data()));  // DPAPI reads it only
    input.cbData = static_cast<DWORD>(secret.size());
    DATA_BLOB entropy = EntropyBlob();
    LocalBlob output;
    if (!::CryptProtectData(&input, L"LDM", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output.blob)) {
        return LastError("CryptProtectData failed");
    }
    return std::vector<std::uint8_t>(output.blob.pbData, output.blob.pbData + output.blob.cbData);
}

base::Result<std::string> DpapiSecretProtector::Unprotect(std::span<const std::uint8_t> protectedSecret)
{
    if (protectedSecret.size() > std::numeric_limits<DWORD>::max()) {
        return base::MakeError(base::ErrorCode::InvalidArgument, "secret too long");
    }
    DATA_BLOB input{};
    input.pbData = const_cast<BYTE*>(protectedSecret.data());  // DPAPI reads it only
    input.cbData = static_cast<DWORD>(protectedSecret.size());
    DATA_BLOB entropy = EntropyBlob();
    LocalBlob output;
    if (!::CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output.blob)) {
        return LastError("CryptUnprotectData failed");
    }
    return std::string(reinterpret_cast<const char*>(output.blob.pbData), output.blob.cbData);
}

}  // namespace ldm::storage
