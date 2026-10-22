#include "platform/win/SystemPorts.h"

#include "base/Paths.h"

#include <bcrypt.h>

#include <array>
#include <cstdio>
#include <system_error>

namespace ldm::platform {

namespace {

std::string Win32ErrorText(DWORD error)
{
    return std::system_category().message(static_cast<int>(error));
}

}  // namespace

std::int64_t SystemClock::NowUnixMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::chrono::steady_clock::time_point SystemClock::Steady()
{
    return std::chrono::steady_clock::now();
}

bool WinFileSystem::Exists(const std::filesystem::path& path)
{
    return ::GetFileAttributesW(base::ToExtendedLengthPath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::optional<std::int64_t> WinFileSystem::FileSize(const std::filesystem::path& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(base::ToExtendedLengthPath(path).c_str(), GetFileExInfoStandard, &data)) {
        return std::nullopt;
    }
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return std::nullopt;
    }
    ULARGE_INTEGER size{};
    size.LowPart = data.nFileSizeLow;
    size.HighPart = data.nFileSizeHigh;
    return static_cast<std::int64_t>(size.QuadPart);
}

base::Status WinFileSystem::Remove(const std::filesystem::path& path)
{
    if (::DeleteFileW(base::ToExtendedLengthPath(path).c_str())) {
        return {};
    }
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return {};
    }
    return base::MakeError(base::ErrorCode::Io, Win32ErrorText(error));
}

base::Status WinFileSystem::CreateDirectories(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(base::ToExtendedLengthPath(path)), error);
    if (error) {
        return base::MakeError(base::ErrorCode::Io, error.message());
    }
    return {};
}

std::string RandomIdGenerator::NewPublicId()
{
    std::array<unsigned char, 16> bytes{};
    const NTSTATUS status = ::BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                                              BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) {
        // The system generator does not fail on supported Windows versions; never fall back to a
        // predictable identifier.
        std::terminate();
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);  // version 4
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);  // RFC 4122 variant
    char text[37] = {};
    std::snprintf(text, sizeof(text), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0],
                  bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10],
                  bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return text;
}

PowerRequest::PowerRequest()
{
    REASON_CONTEXT reason{};
    reason.Version = POWER_REQUEST_CONTEXT_VERSION;
    reason.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    reason.Reason.SimpleReasonString = const_cast<wchar_t*>(L"Last Download Manager is downloading files");  // the API takes LPWSTR but does not write
    request_ = ::PowerCreateRequest(&reason);
}

PowerRequest::~PowerRequest()
{
    if (request_ != INVALID_HANDLE_VALUE) {
        if (active_) {
            ::PowerClearRequest(request_, PowerRequestSystemRequired);
        }
        ::CloseHandle(request_);
    }
}

void PowerRequest::SetTransfersActive(bool active)
{
    if (request_ == INVALID_HANDLE_VALUE || active == active_) {
        return;
    }
    const BOOL changed = active ? ::PowerSetRequest(request_, PowerRequestSystemRequired)
                                : ::PowerClearRequest(request_, PowerRequestSystemRequired);
    if (changed) {
        active_ = active;
    }
}

}  // namespace ldm::platform
