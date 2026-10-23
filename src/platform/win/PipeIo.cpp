#include "platform/win/PipeIo.h"

#include <sddl.h>

#include <cstddef>
#include <cwctype>
#include <vector>

namespace ldm::platform {

namespace {

constexpr size_t kMaxPipeSuffix = 32;

std::wstring TestPipeSuffix()
{
    wchar_t buffer[kMaxPipeSuffix + 2] = {};
    const DWORD length = ::GetEnvironmentVariableW(L"LDM_PIPE_SUFFIX", buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length > kMaxPipeSuffix) {
        return {};
    }
    const std::wstring suffix(buffer, length);
    for (const wchar_t c : suffix) {
        if (!std::iswalnum(c) || c > 0x7F) {
            return {};
        }
    }
    return L"-" + suffix;
}

// Overlapped read or write of exactly `size` bytes.
bool TransferAll(HANDLE pipe, void* data, DWORD size, bool write, HANDLE stopEvent, DWORD timeoutMs)
{
    auto* bytes = static_cast<std::byte*>(data);
    DWORD done = 0;
    OwnedHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.Valid()) {
        return false;
    }
    while (done < size) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.Get();
        DWORD chunk = 0;
        const BOOL started = write ? ::WriteFile(pipe, bytes + done, size - done, &chunk, &overlapped)
                                   : ::ReadFile(pipe, bytes + done, size - done, &chunk, &overlapped);
        if (!started && ::GetLastError() != ERROR_IO_PENDING) {
            return false;
        }
        if (!started) {
            const HANDLE waits[2] = {event.Get(), stopEvent};
            const DWORD count = stopEvent != nullptr ? 2 : 1;
            if (::WaitForMultipleObjects(count, waits, FALSE, timeoutMs) != WAIT_OBJECT_0) {
                ::CancelIoEx(pipe, &overlapped);
                ::GetOverlappedResult(pipe, &overlapped, &chunk, TRUE);
                return false;
            }
            if (!::GetOverlappedResult(pipe, &overlapped, &chunk, FALSE)) {
                return false;
            }
        }
        if (chunk == 0) {
            return false;
        }
        done += chunk;
    }
    return true;
}

}  // namespace

OwnedHandle& OwnedHandle::operator=(OwnedHandle&& other) noexcept
{
    if (this != &other) {
        Reset();
        handle_ = other.Release();
    }
    return *this;
}

HANDLE OwnedHandle::Release() noexcept
{
    const HANDLE handle = handle_;
    handle_ = INVALID_HANDLE_VALUE;
    return handle;
}

void OwnedHandle::Reset() noexcept
{
    if (Valid()) {
        ::CloseHandle(handle_);
    }
    handle_ = INVALID_HANDLE_VALUE;
}

std::wstring CurrentUserSid()
{
    HANDLE rawToken = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        return {};
    }
    OwnedHandle token(rawToken);
    DWORD size = 0;
    ::GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buffer(size);
    if (size == 0 || !::GetTokenInformation(token.Get(), TokenUser, buffer.data(), size, &size)) {
        return {};
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());  // the buffer holds a TOKEN_USER
    wchar_t* text = nullptr;
    if (!::ConvertSidToStringSidW(user->User.Sid, &text)) {
        return {};
    }
    std::wstring sid(text);
    ::LocalFree(text);
    return sid;
}

std::wstring SessionAndUserSuffix()
{
    DWORD session = 0;
    ::ProcessIdToSessionId(::GetCurrentProcessId(), &session);
    return std::to_wstring(session) + L"-" + CurrentUserSid();
}

std::wstring InstancePipeName()
{
    return L"\\\\.\\pipe\\LastDownloadManager-" + SessionAndUserSuffix();
}

std::wstring BrowserPipeName()
{
    return InstancePipeName() + L"-browser" + TestPipeSuffix();
}

OwnerOnlySecurity::OwnerOnlySecurity()
{
    const std::wstring sid = CurrentUserSid();
    if (sid.empty()) {
        return;
    }
    const std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")";
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr)) {
        descriptor_ = nullptr;
        return;
    }
    attributes_.nLength = sizeof(attributes_);
    attributes_.lpSecurityDescriptor = descriptor_;
    attributes_.bInheritHandle = FALSE;
}

OwnerOnlySecurity::~OwnerOnlySecurity()
{
    if (descriptor_ != nullptr) {
        ::LocalFree(descriptor_);
    }
}

std::uint32_t DecodeFrameLength(const unsigned char (&header)[4]) noexcept
{
    std::uint32_t length = 0;
    for (int i = 0; i < 4; ++i) {
        length |= static_cast<std::uint32_t>(header[i]) << (8 * i);
    }
    return length;
}

void EncodeFrameLength(std::uint32_t length, unsigned char (&header)[4]) noexcept
{
    for (int i = 0; i < 4; ++i) {
        header[i] = static_cast<unsigned char>((length >> (8 * i)) & 0xFF);
    }
}

bool WriteFrame(HANDLE pipe, std::string_view payload, std::uint32_t maxBytes, HANDLE stopEvent, DWORD timeoutMs)
{
    if (payload.empty() || payload.size() > maxBytes) {
        return false;
    }
    unsigned char header[4] = {};
    const auto length = static_cast<std::uint32_t>(payload.size());
    EncodeFrameLength(length, header);
    // WriteFile does not modify the buffer; TransferAll takes a mutable pointer for reads.
    return TransferAll(pipe, header, 4, true, stopEvent, timeoutMs) &&
           TransferAll(pipe, const_cast<char*>(payload.data()), length, true, stopEvent, timeoutMs);
}

std::optional<std::string> ReadFrame(HANDLE pipe, std::uint32_t maxBytes, HANDLE stopEvent, DWORD timeoutMs)
{
    unsigned char header[4] = {};
    if (!TransferAll(pipe, header, 4, false, stopEvent, timeoutMs)) {
        return std::nullopt;
    }
    const std::uint32_t length = DecodeFrameLength(header);
    if (length == 0 || length > maxBytes) {
        return std::nullopt;
    }
    std::string payload(length, '\0');
    if (!TransferAll(pipe, payload.data(), length, false, stopEvent, timeoutMs)) {
        return std::nullopt;
    }
    return payload;
}

}  // namespace ldm::platform
