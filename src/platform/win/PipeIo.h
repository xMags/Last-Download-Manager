#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Named-pipe plumbing shared by ldm.exe (instance and browser pipes) and ldm-host.exe. Links only
// ldm_base, so the host stays small. Messages are framed as a 4-byte little-endian length followed
// by that many bytes (design sections 7 of M2 and 3 of M3).
namespace ldm::platform {

// Owns a kernel handle; INVALID_HANDLE_VALUE and null both mean "none".
class OwnedHandle {
public:
    OwnedHandle() = default;
    explicit OwnedHandle(HANDLE handle) noexcept : handle_(handle) {}
    OwnedHandle(OwnedHandle&& other) noexcept : handle_(other.Release()) {}
    OwnedHandle& operator=(OwnedHandle&& other) noexcept;
    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;
    ~OwnedHandle() { Reset(); }

    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
    [[nodiscard]] bool Valid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
    HANDLE Release() noexcept;
    void Reset() noexcept;

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

// The current user's SID as text, or empty when Windows refused.
[[nodiscard]] std::wstring CurrentUserSid();

// "<session>-<user SID>": names objects that must be unique per session and user.
[[nodiscard]] std::wstring SessionAndUserSuffix();

// Pipe names are per session and per user, so two users on one machine never meet.
[[nodiscard]] std::wstring InstancePipeName();
// The pipe ldm-host.exe connects to. Tests set LDM_PIPE_SUFFIX (letters and digits, at most 32)
// to get a private name while the user's LDM keeps running; anything else in it is ignored.
[[nodiscard]] std::wstring BrowserPipeName();

// Whether processes of the same user but a lower integrity level may open the object. Windows
// gives a pipe no integrity label of its own, so without one an LDM running as administrator
// accepts connections from any program at normal level (M3 design section 5).
enum class LowerIntegrity { Allowed, Refused };

// Security attributes that let only the current user open the object.
class OwnerOnlySecurity {
public:
    explicit OwnerOnlySecurity(LowerIntegrity lower = LowerIntegrity::Allowed);
    ~OwnerOnlySecurity();
    OwnerOnlySecurity(const OwnerOnlySecurity&) = delete;
    OwnerOnlySecurity& operator=(const OwnerOnlySecurity&) = delete;

    [[nodiscard]] bool Valid() const noexcept { return descriptor_ != nullptr; }
    [[nodiscard]] SECURITY_ATTRIBUTES* Attributes() noexcept { return &attributes_; }

private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{};
};

inline constexpr DWORD kDefaultPipeTimeoutMs = 3000;

// Writes one framed message to an overlapped pipe handle. False when the message exceeds
// `maxBytes`, the pipe broke, `stopEvent` (optional) was set or `timeoutMs` passed.
bool WriteFrame(HANDLE pipe, std::string_view payload, std::uint32_t maxBytes, HANDLE stopEvent,
                DWORD timeoutMs = kDefaultPipeTimeoutMs);
// Reads one framed message. Nothing on the same conditions, or when the announced length is zero
// or above `maxBytes` (the sender is not trusted).
[[nodiscard]] std::optional<std::string> ReadFrame(HANDLE pipe, std::uint32_t maxBytes, HANDLE stopEvent,
                                                   DWORD timeoutMs = kDefaultPipeTimeoutMs);

// Little-endian length prefix helpers, also used for native messaging (x64 is little-endian).
[[nodiscard]] std::uint32_t DecodeFrameLength(const unsigned char (&header)[4]) noexcept;
void EncodeFrameLength(std::uint32_t length, unsigned char (&header)[4]) noexcept;

}  // namespace ldm::platform
