#include "platform/win/InstanceChannel.h"

#include "base/Utf.h"
#include "core/FileNames.h"
#include "core/Url.h"

#include <nlohmann/json.hpp>
#include <sddl.h>

#include <array>
#include <filesystem>
#include <vector>

namespace ldm::platform {

namespace {

constexpr DWORD kIoTimeoutMs = 3000;
constexpr size_t kMaxFieldBytes = 32 * 1024;

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE handle) noexcept : handle_(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle()
    {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
    }
    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
    [[nodiscard]] bool Valid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::wstring UserSid()
{
    HANDLE rawToken = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        return {};
    }
    Handle token(rawToken);
    DWORD size = 0;
    ::GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buffer(size);
    if (!::GetTokenInformation(token.Get(), TokenUser, buffer.data(), size, &size)) {
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

std::wstring SessionSuffix()
{
    DWORD session = 0;
    ::ProcessIdToSessionId(::GetCurrentProcessId(), &session);
    return std::to_wstring(session) + L"-" + UserSid();
}

std::wstring PipeName()
{
    return L"\\\\.\\pipe\\LastDownloadManager-" + SessionSuffix();
}

// Only the current user may open the pipe.
struct OwnerOnlySecurity {
    OwnerOnlySecurity()
    {
        const std::wstring sddl = L"D:P(A;;GA;;;" + UserSid() + L")";
        ::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr);
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = descriptor;
        attributes.bInheritHandle = FALSE;
    }
    ~OwnerOnlySecurity()
    {
        if (descriptor != nullptr) {
            ::LocalFree(descriptor);
        }
    }
    OwnerOnlySecurity(const OwnerOnlySecurity&) = delete;
    OwnerOnlySecurity& operator=(const OwnerOnlySecurity&) = delete;

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{};
};

// Overlapped read or write of exactly `size` bytes, giving up at the timeout or the stop event.
bool TransferAll(HANDLE pipe, void* data, DWORD size, bool write, HANDLE stopEvent)
{
    auto* bytes = static_cast<std::byte*>(data);
    DWORD done = 0;
    Handle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
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
            if (::WaitForMultipleObjects(count, waits, FALSE, kIoTimeoutMs) != WAIT_OBJECT_0) {
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

bool WriteMessage(HANDLE pipe, const std::string& payload, HANDLE stopEvent)
{
    if (payload.size() > kMaxMessageBytes) {
        return false;
    }
    std::array<std::byte, 4> header{};
    const auto length = static_cast<std::uint32_t>(payload.size());
    for (int i = 0; i < 4; ++i) {
        header[static_cast<size_t>(i)] = static_cast<std::byte>((length >> (8 * i)) & 0xFF);
    }
    return TransferAll(pipe, header.data(), 4, true, stopEvent) &&
           TransferAll(pipe, const_cast<char*>(payload.data()), length, true, stopEvent);  // WriteFile does not modify it
}

std::optional<std::string> ReadMessage(HANDLE pipe, HANDLE stopEvent)
{
    std::array<std::byte, 4> header{};
    if (!TransferAll(pipe, header.data(), 4, false, stopEvent)) {
        return std::nullopt;
    }
    std::uint32_t length = 0;
    for (int i = 0; i < 4; ++i) {
        length |= static_cast<std::uint32_t>(header[static_cast<size_t>(i)]) << (8 * i);
    }
    if (length == 0 || length > kMaxMessageBytes) {
        return std::nullopt;
    }
    std::string payload(length, '\0');
    if (!TransferAll(pipe, payload.data(), length, false, stopEvent)) {
        return std::nullopt;
    }
    return payload;
}

std::optional<std::string> OptionalString(const nlohmann::json& object, const char* key, bool& valid)
{
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return std::nullopt;
    }
    if (!it->is_string() || it->get_ref<const std::string&>().size() > kMaxFieldBytes) {
        valid = false;
        return std::nullopt;
    }
    return it->get<std::string>();
}

bool Flag(const nlohmann::json& object, const char* key, bool& valid)
{
    const auto it = object.find(key);
    if (it == object.end()) {
        return false;
    }
    if (!it->is_boolean()) {
        valid = false;
        return false;
    }
    return it->get<bool>();
}

}  // namespace

std::string EncodeLaunchRequest(const core::LaunchRequest& request, std::string_view requestId)
{
    nlohmann::json message = {
        {"protocol", kProtocolVersion},
        {"id", std::string(requestId)},
        {"type", "launch"},
        {"silent", request.silent},
        {"queueOnly", request.queueOnly},
        {"startQueue", request.startQueue},
        {"tray", request.tray},
    };
    if (request.url) {
        message["url"] = *request.url;
    }
    if (request.saveDir) {
        message["saveDir"] = *request.saveDir;
    }
    if (request.fileName) {
        message["fileName"] = *request.fileName;
    }
    return message.dump();
}

std::optional<core::LaunchRequest> DecodeLaunchRequest(std::string_view json)
{
    const nlohmann::json message = nlohmann::json::parse(json, nullptr, false);
    if (message.is_discarded() || !message.is_object()) {
        return std::nullopt;
    }
    const auto protocol = message.find("protocol");
    const auto type = message.find("type");
    const auto id = message.find("id");
    if (protocol == message.end() || !protocol->is_number_integer() || protocol->get<int>() != kProtocolVersion ||
        type == message.end() || !type->is_string() || type->get<std::string>() != "launch" || id == message.end() ||
        !id->is_string() || id->get_ref<const std::string&>().size() > 64) {
        return std::nullopt;
    }
    bool valid = true;
    core::LaunchRequest request;
    request.url = OptionalString(message, "url", valid);
    request.saveDir = OptionalString(message, "saveDir", valid);
    request.fileName = OptionalString(message, "fileName", valid);
    request.silent = Flag(message, "silent", valid);
    request.queueOnly = Flag(message, "queueOnly", valid);
    request.startQueue = Flag(message, "startQueue", valid);
    request.tray = Flag(message, "tray", valid);
    if (!valid) {
        return std::nullopt;
    }
    // The same rules as the command line: the sender is another process and is not trusted.
    if (request.url && !core::ParseUrl(*request.url)) {
        return std::nullopt;
    }
    if (request.saveDir && !std::filesystem::path(base::ToUtf16(*request.saveDir)).is_absolute()) {
        return std::nullopt;
    }
    if (request.fileName && (request.fileName->empty() || core::SanitizeFileName(*request.fileName) != *request.fileName)) {
        return std::nullopt;
    }
    return request;
}

std::unique_ptr<InstanceLock> InstanceLock::TryAcquire()
{
    const std::wstring name = L"Local\\LastDownloadManager.Instance-" + SessionSuffix();
    HANDLE mutex = ::CreateMutexW(nullptr, TRUE, name.c_str());
    if (mutex == nullptr) {
        return nullptr;
    }
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(mutex);
        return nullptr;
    }
    return std::unique_ptr<InstanceLock>(new InstanceLock(mutex));  // private constructor
}

InstanceLock::~InstanceLock()
{
    if (mutex_ != nullptr) {
        ::ReleaseMutex(mutex_);
        ::CloseHandle(mutex_);
    }
}

InstanceServer::InstanceServer(std::function<void(core::LaunchRequest)> onRequest) : onRequest_(std::move(onRequest)) {}

InstanceServer::~InstanceServer()
{
    if (stopEvent_ != nullptr) {
        ::SetEvent(stopEvent_);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (firstPipe_ != INVALID_HANDLE_VALUE) {
        ::CloseHandle(firstPipe_);
    }
    if (stopEvent_ != nullptr) {
        ::CloseHandle(stopEvent_);
    }
}

bool InstanceServer::Start()
{
    stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OwnerOnlySecurity security;
    if (stopEvent_ == nullptr || security.descriptor == nullptr) {
        return false;
    }
    // FILE_FLAG_FIRST_PIPE_INSTANCE fails if anyone created the name first, so another program
    // cannot pose as LDM and receive the commands.
    firstPipe_ = ::CreateNamedPipeW(PipeName().c_str(),
                                    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                    kMaxMessageBytes, kMaxMessageBytes, 0, &security.attributes);
    if (firstPipe_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    thread_ = std::thread([this] { Run(); });
    return true;
}

void InstanceServer::Run()
{
    Handle connected(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const HANDLE pipe = firstPipe_;
    while (true) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = connected.Get();
        ::ResetEvent(connected.Get());
        bool ready = false;
        if (::ConnectNamedPipe(pipe, &overlapped)) {
            ready = true;
        } else if (::GetLastError() == ERROR_PIPE_CONNECTED) {
            ready = true;
        } else if (::GetLastError() == ERROR_IO_PENDING) {
            const HANDLE waits[2] = {connected.Get(), stopEvent_};
            if (::WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
                ::CancelIoEx(pipe, &overlapped);
                DWORD ignored = 0;
                ::GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
                return;  // stop requested
            }
            DWORD ignored = 0;
            ready = ::GetOverlappedResult(pipe, &overlapped, &ignored, FALSE) != FALSE;
        }
        if (::WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) {
            return;
        }
        if (ready) {
            if (const auto payload = ReadMessage(pipe, stopEvent_)) {
                const nlohmann::json parsed = nlohmann::json::parse(*payload, nullptr, false);
                const std::string id = parsed.is_object() && parsed.contains("id") && parsed["id"].is_string()
                                           ? parsed["id"].get<std::string>()
                                           : std::string();
                const auto request = DecodeLaunchRequest(*payload);
                const nlohmann::json answer = {{"protocol", kProtocolVersion}, {"id", id}, {"ok", request.has_value()}};
                WriteMessage(pipe, answer.dump(), stopEvent_);
                if (request) {
                    onRequest_(*request);
                }
            }
        }
        ::FlushFileBuffers(pipe);
        ::DisconnectNamedPipe(pipe);
    }
}

bool ForwardToRunningInstance(const core::LaunchRequest& request)
{
    const std::wstring name = PipeName();
    for (int attempt = 0; attempt < 10; ++attempt) {
        Handle pipe(::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        if (!pipe.Valid()) {
            if (::GetLastError() != ERROR_PIPE_BUSY && ::GetLastError() != ERROR_FILE_NOT_FOUND) {
                return false;
            }
            // Busy with another launch, or still starting: wait a little and try again.
            ::WaitNamedPipeW(name.c_str(), 300);
            ::Sleep(100);
            continue;
        }
        const std::string id = std::to_string(::GetCurrentProcessId());
        if (!WriteMessage(pipe.Get(), EncodeLaunchRequest(request, id), nullptr)) {
            return false;
        }
        const auto answer = ReadMessage(pipe.Get(), nullptr);
        if (!answer) {
            return false;
        }
        const nlohmann::json parsed = nlohmann::json::parse(*answer, nullptr, false);
        return parsed.is_object() && parsed.value("ok", false) && parsed.value("id", std::string()) == id;
    }
    return false;
}

void AllowRunningInstanceToActivate()
{
    ::AllowSetForegroundWindow(ASFW_ANY);
}

}  // namespace ldm::platform
